/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/static.cpp
 * - Non-inferred type checking
 */
#include "static.hpp"
#include <algorithm>

void StaticTraitResolve::prep_indexes()
{
    static Span sp_AAA;
    const Span& sp = sp_AAA;

    TRACE_FUNCTION_F("");

    m_copy_cache.clear();

    auto add_equality = [&](::HIR::TypeRef long_ty, ::HIR::TypeRef short_ty){
        DEBUG("[prep_indexes] ADD " << long_ty << " => " << short_ty);
        // TODO: Sort the two types by "complexity" (most of the time long >= short)
        this->m_type_equalities.insert(::std::make_pair( mv$(long_ty), mv$(short_ty) ));
        };

    this->iterate_bounds([&](const auto& b) {
        TU_MATCH_DEF(::HIR::GenericBound, (b), (be),
        (
            ),
        (TraitBound,
            DEBUG("[prep_indexes] `" << be.type << " : " << be.trait);
            for( const auto& tb : be.trait.m_type_bounds ) {
                DEBUG("[prep_indexes] Equality (TB) - <" << be.type << " as " << be.trait.m_path << ">::" << tb.first << " = " << tb.second);
                auto ty_l = ::HIR::TypeRef( ::HIR::Path( be.type.clone(), be.trait.m_path.clone(), tb.first ) );
                ty_l.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});

                add_equality( mv$(ty_l), tb.second.clone() );
            }

            const auto& trait_params = be.trait.m_path.m_params;
            auto cb_mono = [&](const auto& ty)->const auto& {
                const auto& ge = ty.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    return be.type;
                }
                else if( ge.binding < 256 ) {
                    unsigned idx = ge.binding % 256;
                    ASSERT_BUG(sp, idx < trait_params.m_types.size(), "Generic binding out of range in trait " << be.trait);
                    return trait_params.m_types[idx];
                }
                else {
                    BUG(sp, "Unknown generic binding " << ty);
                }
                };

            const auto& trait = m_crate.get_trait_by_path(sp, be.trait.m_path.m_path);
            for(const auto& a_ty : trait.m_types)
            {
                ::HIR::TypeRef ty_a;
                for( const auto& a_ty_b : a_ty.second.m_trait_bounds ) {
                    DEBUG("[prep_indexes] (Assoc) " << a_ty_b);
                    auto trait_mono = monomorphise_traitpath_with(sp, a_ty_b, cb_mono, false);
                    for( auto& tb : trait_mono.m_type_bounds ) {
                        if( ty_a == ::HIR::TypeRef() ) {
                            ty_a = ::HIR::TypeRef( ::HIR::Path( be.type.clone(), be.trait.m_path.clone(), a_ty.first ) );
                            ty_a.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                        }
                        DEBUG("[prep_indexes] Equality (ATB) - <" << ty_a << " as " << a_ty_b.m_path << ">::" << tb.first << " = " << tb.second);

                        auto ty_l = ::HIR::TypeRef( ::HIR::Path( ty_a.clone(), trait_mono.m_path.clone(), tb.first ) );
                        ty_l.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});

                        add_equality( mv$(ty_l), mv$(tb.second) );
                    }
                }
            }
            ),
        (TypeEquality,
            DEBUG("Equality - " << be.type << " = " << be.other_type);
            add_equality( be.type.clone(), be.other_type.clone() );
            )
        )
        return false;
        });
}

bool StaticTraitResolve::find_impl(
    const Span& sp,
    const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
    const ::HIR::TypeRef& type,
    t_cb_find_impl found_cb,
    bool dont_handoff_to_specialised
    ) const
{
    TRACE_FUNCTION_F(trait_path << FMT_CB(os, if(trait_params) { os << *trait_params; } else { os << "<?>"; }) << " for " << type);
    auto cb_ident = [](const auto&ty)->const auto&{return ty;};

    static ::HIR::PathParams    null_params;
    static ::std::map< ::std::string, ::HIR::TypeRef>    null_assoc;

    if( !dont_handoff_to_specialised ) {
        if( trait_path == m_lang_Copy ) {
            if( this->type_is_copy(sp, type) ) {
                return found_cb( ImplRef(&type, &null_params, &null_assoc), false );
            }
        }
        else if( trait_path == m_lang_Sized ) {
            if( this->type_is_sized(sp, type) ) {
                return found_cb( ImplRef(&type, &null_params, &null_assoc), false );
            }
        }
        //else if( trait_path == m_lang_Unsize ) {
        //    if( true ) {
        //        return found_cb( ImplRef(&type, &null_params, &null_assoc), false );
        //    }
        //}
    }

    // --- MAGIC IMPLS ---
    // TODO: There should be quite a few more here, but laziness
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Function, e,
        if( trait_path == m_lang_Fn || trait_path == m_lang_FnMut || trait_path == m_lang_FnOnce ) {
            if( trait_params )
            {
                const auto& des_arg_tys = trait_params->m_types.at(0).m_data.as_Tuple();
                if( des_arg_tys.size() != e.m_arg_types.size() ) {
                    return false;
                }
                for(unsigned int i = 0; i < des_arg_tys.size(); i ++)
                {
                    if( des_arg_tys[i] != e.m_arg_types[i] ) {
                        return false;
                    }
                }
            }
            else
            {
                trait_params = &null_params;
            }
            ::std::map< ::std::string, ::HIR::TypeRef>  assoc;
            assoc.insert( ::std::make_pair("Output", e.m_rettype->clone()) );
            return found_cb( ImplRef(type.clone(), trait_params->clone(), mv$(assoc)), false );
        }
    )

    // ----
    // TraitObject traits and supertraits
    // ----
    TU_IFLET( ::HIR::TypeRef::Data, type.m_data, TraitObject, e,
        if( trait_path == e.m_trait.m_path.m_path )
        {
            if( !trait_params || e.m_trait.m_path.m_params == *trait_params )
            {
                return found_cb( ImplRef(&type, &e.m_trait.m_path.m_params, &e.m_trait.m_type_bounds), false );
            }
        }
        // Markers too
        for( const auto& mt : e.m_markers )
        {
            if( trait_path == mt.m_path ) {
                if( !trait_params || mt.m_params == *trait_params )
                {
                    static ::std::map< ::std::string, ::HIR::TypeRef>  types;
                    return found_cb( ImplRef(&type, &mt.m_params, &types), false );
                }
            }
        }

        // - Check if the desired trait is a supertrait of this.
        // TODO: What if `trait_params` is nullptr?
        bool rv = false;
        bool is_supertrait = trait_params && this->find_named_trait_in_trait(sp, trait_path,*trait_params, *e.m_trait.m_trait_ptr, e.m_trait.m_path.m_path,e.m_trait.m_path.m_params, type,
            [&](const auto& i_params, const auto& i_assoc) {
                // Invoke callback with a proper ImplRef
                ::std::map< ::std::string, ::HIR::TypeRef> assoc_clone;
                for(const auto& e : i_assoc)
                    assoc_clone.insert( ::std::make_pair(e.first, e.second.clone()) );
                // HACK! Just add all the associated type bounds (only inserted if not already present)
                for(const auto& e2 : e.m_trait.m_type_bounds)
                    assoc_clone.insert( ::std::make_pair(e2.first, e2.second.clone()) );
                auto ir = ImplRef(type.clone(), i_params.clone(), mv$(assoc_clone));
                DEBUG("- ir = " << ir);
                rv = found_cb( mv$(ir), false );
                return false;
            });
        if( is_supertrait )
        {
            return rv;
        }
    )
    // --- / ---
    TU_IFLET( ::HIR::TypeRef::Data, type.m_data, ErasedType, e,
        for(const auto& trait : e.m_traits)
        {
            if( trait_path == trait.m_path.m_path && (!trait_params || trait.m_path.m_params == *trait_params) )
            {
                return found_cb( ImplRef(&type, &trait.m_path.m_params, &trait.m_type_bounds), false );
            }

            // TODO: What if `trait_params` is nullptr?
            bool rv = false;
            bool is_supertrait = trait_params && this->find_named_trait_in_trait(sp, trait_path,*trait_params, *trait.m_trait_ptr, trait.m_path.m_path,trait.m_path.m_params, type,
                [&](const auto& i_params, const auto& i_assoc) {
                    // Invoke callback with a proper ImplRef
                    ::std::map< ::std::string, ::HIR::TypeRef> assoc_clone;
                    for(const auto& e : i_assoc)
                        assoc_clone.insert( ::std::make_pair(e.first, e.second.clone()) );
                    // HACK! Just add all the associated type bounds (only inserted if not already present)
                    for(const auto& e2 : trait.m_type_bounds)
                        assoc_clone.insert( ::std::make_pair(e2.first, e2.second.clone()) );
                    auto ir = ImplRef(type.clone(), i_params.clone(), mv$(assoc_clone));
                    DEBUG("- ir = " << ir);
                    rv = found_cb( mv$(ir), false );
                    return false;
                });
            if( is_supertrait )
            {
                return rv;
            }
        }
    )

    // ---
    // If this type is an opaque UfcsKnown - check bounds
    // ---
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Path, e,
        if( e.binding.is_Opaque() )
        {
            ASSERT_BUG(sp, e.path.m_data.is_UfcsKnown(), "Opaque bound type wasn't UfcsKnown - " << type);
            const auto& pe = e.path.m_data.as_UfcsKnown();
            DEBUG("Checking bounds on definition of " << pe.item << " in " << pe.trait);

            // If this associated type has a bound of the desired trait, return it.
            const auto& trait_ref = m_crate.get_trait_by_path(sp, pe.trait.m_path);
            ASSERT_BUG(sp, trait_ref.m_types.count( pe.item ) != 0, "Trait " << pe.trait.m_path << " doesn't contain an associated type " << pe.item);
            const auto& aty_def = trait_ref.m_types.find(pe.item)->second;

            auto monomorph_cb = monomorphise_type_get_cb(sp, &*pe.type, &pe.trait.m_params, nullptr, nullptr);

            for(const auto& bound : aty_def.m_trait_bounds)
            {
                const auto& b_params = bound.m_path.m_params;
                ::HIR::PathParams   params_mono_o;
                const auto& b_params_mono = (monomorphise_pathparams_needed(b_params) ? params_mono_o = monomorphise_path_params_with(sp, b_params, monomorph_cb, false) : b_params);
                DEBUG(": " << bound.m_path.m_path << b_params_mono);

                if( bound.m_path.m_path == trait_path )
                {
                    if( !trait_params || b_params_mono == *trait_params )
                    {
                        if( &b_params_mono == &params_mono_o || ::std::any_of(bound.m_type_bounds.begin(), bound.m_type_bounds.end(), [&](const auto& x){ return monomorphise_type_needed(x.second); }) )
                        {
                            ::std::map< ::std::string, ::HIR::TypeRef>  atys;
                            if( ! bound.m_type_bounds.empty() )
                            {
                                for(const auto& tb : bound.m_type_bounds)
                                {
                                    auto aty = monomorphise_type_with(sp, tb.second, monomorph_cb, false);
                                    expand_associated_types(sp, aty);
                                    atys.insert(::std::make_pair( tb.first, mv$(aty) ));
                                }
                            }
                            if( found_cb( ImplRef(type.clone(), mv$(params_mono_o), mv$(atys)), false ) )
                                return true;
                            params_mono_o = monomorphise_path_params_with(sp, b_params, monomorph_cb, false);
                        }
                        else
                        {
                            if( found_cb( ImplRef(&type, &bound.m_path.m_params, &bound.m_type_bounds), false ) )
                                return true;
                        }
                    }
                }

                bool ret = trait_params && this->find_named_trait_in_trait(sp,  trait_path, *trait_params,  *bound.m_trait_ptr,  bound.m_path.m_path, b_params_mono, type,
                    [&](const auto& i_params, const auto& i_assoc) {
                        if( i_params != *trait_params )
                            return false;
                        DEBUG("impl " << trait_path << i_params << " for " << type << " -- desired " << trait_path << *trait_params);
                        return found_cb( ImplRef(type.clone(), i_params.clone(), {}), false );
                    });
                if( ret )
                    return true;
            }
            DEBUG("- No bounds matched");
        }
    )
    // --- /UfcsKnown ---

    bool ret;

    // TODO: A bound can imply something via its associated types. How deep can this go?
    // E.g. `T: IntoIterator<Item=&u8>` implies `<T as IntoIterator>::IntoIter : Iterator<Item=&u8>`
    ret = this->iterate_bounds([&](const auto& b) {
        return this->find_impl__check_bound(sp, trait_path, trait_params, type, found_cb,  b);
        });
    if(ret)
        return true;

    if( m_crate.get_trait_by_path(sp, trait_path).m_is_marker )
    {
        // Positive/negative impls
        bool rv = false;
        ret = this->m_crate.find_auto_trait_impls(trait_path, type, cb_ident, [&](const auto& impl) {
            DEBUG("[find_impls] - Auto " << (impl.is_positive?"Pos":"Neg")
                << " impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type << " " << impl.m_params.fmt_bounds());
            return this->find_impl__check_crate_raw(sp, trait_path, trait_params, type, impl.m_params, impl.m_trait_args, impl.m_type,
                [&](auto impl_params, auto placeholders, auto cmp) {
                    if( impl.is_positive ) {
                        //rv = found_cb( ImplRef(impl_params, trait_path, impl, mv$(placeholders)), (match == ::HIR::Compare::Fuzzy) );
                        rv = found_cb( ImplRef(&type, trait_params, &null_assoc), cmp == ::HIR::Compare::Fuzzy );
                        return rv;
                    }
                    else {
                        rv = false;
                        return true;
                    }
                });
            });
        if(ret)
            return rv;

        // Detect recursion and return true if detected
        static ::std::vector< ::std::tuple< const ::HIR::SimplePath*, const ::HIR::PathParams*, const ::HIR::TypeRef*> >    stack;
        for(const auto& ent : stack ) {
            if( *::std::get<0>(ent) != trait_path )
                continue ;
            if( ::std::get<1>(ent) && trait_params && *::std::get<1>(ent) != *trait_params )
                continue ;
            if( *::std::get<2>(ent) != type )
                continue ;

            return found_cb( ImplRef(&type, trait_params, &null_assoc), false );
        }
        stack.push_back( ::std::make_tuple( &trait_path, trait_params, &type ) );
        struct Guard {
            ~Guard() { stack.pop_back(); }
        };
        Guard   _;

        auto cmp = this->check_auto_trait_impl_destructure(sp, trait_path, trait_params, type);
        if( cmp != ::HIR::Compare::Unequal )
            return found_cb( ImplRef(&type, trait_params, &null_assoc), cmp == ::HIR::Compare::Fuzzy );
        return false;
    }
    else
    {
        // Search the crate for impls
        ret = m_crate.find_trait_impls(trait_path, type, cb_ident, [&](const auto& impl) {
            return this->find_impl__check_crate(sp, trait_path, trait_params, type, found_cb,  impl);
            });
        if(ret)
            return true;

        return false;
    }
}

bool StaticTraitResolve::find_impl__check_bound(
        const Span& sp,
        const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb,
        const ::HIR::GenericBound& bound
    ) const
{
    struct H {
        static bool compare_pp(const Span& sp, const ::HIR::PathParams& left, const ::HIR::PathParams& right) {
            ASSERT_BUG( sp, left.m_types.size() == right.m_types.size(), "Parameter count mismatch" );
            for(unsigned int i = 0; i < left.m_types.size(); i ++) {
                if( left.m_types[i] != right.m_types[i] ) {
                    return false;
                }
            }
            return true;
        }
    };

    // Can only get good information out of TraitBound
    if( !bound.is_TraitBound() ) {
        return false;
    }
    const auto& e = bound.as_TraitBound();

    // Obtain a pointer to UfcsKnown for magic later
    const ::HIR::Path::Data::Data_UfcsKnown* assoc_info = nullptr;
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Path, e,
        TU_IFLET(::HIR::Path::Data, e.path.m_data, UfcsKnown, pe,
            assoc_info = &pe;
        )
    )

    const auto& b_params = e.trait.m_path.m_params;
    DEBUG("(bound) - " << e.type << " : " << e.trait);
    if( e.type == type )
    {
        if( e.trait.m_path.m_path == trait_path ) {
            // Check against `params`
            if( trait_params ) {
                DEBUG("Checking " << *trait_params << " vs " << b_params);
                if( !H::compare_pp(sp, *trait_params, b_params) )
                    return false;
            }
            // Hand off to the closure, and return true if it does
            if( found_cb(ImplRef(&e.type, &e.trait.m_path.m_params, &e.trait.m_type_bounds), false) ) {
                return true;
            }
        }
        // HACK: The wrapping closure takes associated types from this bound and applies them to the returned set
        // - XXX: This is actually wrong (false-positive) in many cases. FIXME
        bool rv = this->find_named_trait_in_trait(sp,
            trait_path,*trait_params,
            *e.trait.m_trait_ptr, e.trait.m_path.m_path,e.trait.m_path.m_params,
            type,
            [&](const auto& params, auto assoc) {
                for(const auto& i : e.trait.m_type_bounds) {
                    // TODO: Only include from above when needed
                    //if( des_trait_ref.m_types.count(i.first) ) {
                        assoc.insert( ::std::make_pair(i.first, i.second.clone())  );
                    //}
                }
                return found_cb( ImplRef(type.clone(), params.clone(), mv$(assoc)), false );
            });
        if( rv ) {
            return true;
        }
    }

    // If the input type is an associated type controlled by this trait bound, check for added bounds.
    // TODO: This just checks a single layer, but it's feasable that there could be multiple layers
    if( assoc_info && e.trait.m_path.m_path == assoc_info->trait.m_path && e.type == *assoc_info->type && H::compare_pp(sp, b_params, assoc_info->trait.m_params) ) {

        const auto& trait_ref = *e.trait.m_trait_ptr;
        const auto& at = trait_ref.m_types.at(assoc_info->item);
        for(const auto& bound : at.m_trait_bounds) {
            if( bound.m_path.m_path == trait_path && (!trait_params || H::compare_pp(sp, bound.m_path.m_params, *trait_params)) ) {
                DEBUG("- Found an associated type impl");

                auto tp_mono = monomorphise_traitpath_with(sp, bound, [&assoc_info,&sp](const auto& gt)->const auto& {
                    const auto& ge = gt.m_data.as_Generic();
                    if( ge.binding == 0xFFFF ) {
                        return *assoc_info->type;
                    }
                    else {
                        if( ge.binding >= assoc_info->trait.m_params.m_types.size() )
                            BUG(sp, "find_trait_impls_bound - Generic #" << ge.binding << " " << ge.name << " out of range");
                        return assoc_info->trait.m_params.m_types[ge.binding];
                    }
                    }, false);
                // - Expand associated types
                for(auto& ty : tp_mono.m_type_bounds) {
                    this->expand_associated_types(sp, ty.second);
                }
                DEBUG("- tp_mono = " << tp_mono);
                // TODO: Instead of using `type` here, build the real type
                if( found_cb( ImplRef(type.clone(), mv$(tp_mono.m_path.m_params), mv$(tp_mono.m_type_bounds)), false ) ) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool StaticTraitResolve::find_impl__check_crate_raw(
        const Span& sp,
        const ::HIR::SimplePath& des_trait_path, const ::HIR::PathParams* des_trait_params, const ::HIR::TypeRef& des_type,
        const ::HIR::GenericParams& impl_params_def, const ::HIR::PathParams& impl_trait_params, const ::HIR::TypeRef& impl_type,
        ::std::function<bool(::std::vector<const ::HIR::TypeRef*>, ::std::vector<::HIR::TypeRef>, ::HIR::Compare)> found_cb
    ) const
{
    auto cb_ident = [](const auto&ty)->const auto&{return ty;};
    TRACE_FUNCTION_F("impl" << impl_params_def.fmt_args() << " " << des_trait_path << impl_trait_params << " for " << impl_type << impl_params_def.fmt_bounds());

    ::std::vector< const ::HIR::TypeRef*> impl_params;
    impl_params.resize( impl_params_def.m_types.size() );

    auto cb = [&impl_params,&sp,cb_ident](auto idx, const auto& ty) {
        assert( idx < impl_params.size() );
        if( ! impl_params[idx] ) {
            impl_params[idx] = &ty;
            return ::HIR::Compare::Equal;
        }
        else {
            return impl_params[idx]->compare_with_placeholders(sp, ty, cb_ident);
        }
        };
    auto match = impl_type.match_test_generics_fuzz(sp, des_type, cb_ident, cb);
    if( des_trait_params )
    {
        assert( des_trait_params->m_types.size() == impl_trait_params.m_types.size() );
        for( unsigned int i = 0; i < impl_trait_params.m_types.size(); i ++ )
        {
            const auto& l = impl_trait_params.m_types[i];
            const auto& r = des_trait_params->m_types[i];
            match &= l.match_test_generics_fuzz(sp, r, cb_ident, cb);
        }
    }
    if( match == ::HIR::Compare::Unequal ) {
        DEBUG(" > Type mismatch");
        return false;
    }

    ::std::vector< ::HIR::TypeRef>  placeholders;
    for(unsigned int i = 0; i < impl_params.size(); i ++ ) {
        if( !impl_params[i] ) {
            if( placeholders.size() == 0 )
                placeholders.resize(impl_params.size());
            placeholders[i] = ::HIR::TypeRef("impl_?", 2*256 + i);
        }
    }
    // Callback that matches placeholders to concrete types
    auto cb_match = [&](unsigned int idx, const auto& ty) {
        if( ty.m_data.is_Generic() && ty.m_data.as_Generic().binding == idx )
            return ::HIR::Compare::Equal;
        if( idx >> 8 == 2 ) {
            auto i = idx % 256;
            ASSERT_BUG(sp, !impl_params[i], "Placeholder to populated type returned");
            auto& ph = placeholders[i];
            if( ph.m_data.is_Generic() && ph.m_data.as_Generic().binding == idx ) {
                DEBUG("[find_impl__check_crate_raw:cb_match] Bind placeholder " << i << " to " << ty);
                ph = ty.clone();
                return ::HIR::Compare::Equal;
            }
            else {
                TODO(sp, "[find_impl__check_crate_raw:cb_match] Compare placeholder " << i << " " << ph << " == " << ty);
            }
        }
        else {
            return ::HIR::Compare::Unequal;
        }
        };
    // Callback that returns monomorpisation results
    auto cb_monomorph = [&](const auto& gt)->const auto& {
            const auto& ge = gt.m_data.as_Generic();
            ASSERT_BUG(sp, ge.binding >> 8 != 2, "");
            assert( ge.binding < impl_params.size() );
            if( !impl_params[ge.binding] ) {
                return placeholders[ge.binding];
            }
            return *impl_params[ge.binding];
            };

    // Bounds
    for(const auto& bound : impl_params_def.m_bounds) {
        TU_MATCH_DEF(::HIR::GenericBound, (bound), (e),
        (
            ),
        (TraitBound,
            DEBUG("[find_impl] Trait bound " << e.type << " : " << e.trait);
            auto b_ty_mono = monomorphise_type_with(sp, e.type, cb_monomorph);
            this->expand_associated_types(sp, b_ty_mono);
            auto b_tp_mono = monomorphise_traitpath_with(sp, e.trait, cb_monomorph, false);
            for(auto& ty : b_tp_mono.m_path.m_params.m_types) {
                this->expand_associated_types(sp, ty);
            }
            for(auto& assoc_bound : b_tp_mono.m_type_bounds) {
                // TODO: These should be tagged with the source trait and that source trait used for expansion.
                this->expand_associated_types(sp, assoc_bound.second);
            }
            DEBUG("[find_impl] - b_ty_mono = " << b_ty_mono << ", b_tp_mono = " << b_tp_mono);
            // HACK: If the type is '_', assume the bound passes
            if( b_ty_mono.m_data.is_Infer() ) {
                continue ;
            }

            // TODO: This is extrememly inefficient (looks up the trait impl 1+N times)
            if( b_tp_mono.m_type_bounds.size() > 0 ) {
                //
                for(const auto& assoc_bound : b_tp_mono.m_type_bounds) {
                    const auto& aty_name = assoc_bound.first;
                    const ::HIR::TypeRef& exp = assoc_bound.second;

                    ::HIR::GenericPath  aty_src_trait;
                    trait_contains_type(sp, b_tp_mono.m_path, *e.trait.m_trait_ptr, aty_name, aty_src_trait);

                    bool rv = false;
                    if( b_ty_mono.m_data.is_Generic() && (b_ty_mono.m_data.as_Generic().binding >> 8) == 2 ) {
                        DEBUG("- Placeholder param " << b_ty_mono << ", magic success");
                        rv = true;
                    }
                    else {
                        rv = this->find_impl(sp, aty_src_trait.m_path, aty_src_trait.m_params, b_ty_mono, [&](const auto& impl, bool) {
                            ::HIR::TypeRef have = impl.get_type(aty_name.c_str());
                            this->expand_associated_types(sp, have);

                            //auto cmp = have .match_test_generics_fuzz(sp, exp, cb_ident, cb_match);
                            auto cmp = exp .match_test_generics_fuzz(sp, have, cb_ident, cb_match);
                            ASSERT_BUG(sp, cmp == ::HIR::Compare::Equal, "Assoc ty " << aty_name << " mismatch, " << have << " != des " << exp);
                            return true;
                            });
                    }
                    if( !rv ) {
                        DEBUG("> Fail (assoc) - " << b_ty_mono << " : " << aty_src_trait);
                        return false;
                    }
                }
            }
            else
            {
                bool rv = false;
                if( b_ty_mono.m_data.is_Generic() && (b_ty_mono.m_data.as_Generic().binding >> 8) == 2 ) {
                    rv = true;
                }
                else {
                    rv = this->find_impl(sp, b_tp_mono.m_path.m_path, b_tp_mono.m_path.m_params, b_ty_mono, [&](const auto& impl, bool) {
                        return true;
                        });
                }
                if( !rv ) {
                    DEBUG("> Fail - " << b_ty_mono << ": " << b_tp_mono);
                    return false;
                }
            }
            )
        )
    }

    return found_cb( mv$(impl_params), mv$(placeholders), match );
}

bool StaticTraitResolve::find_impl__check_crate(
        const Span& sp,
        const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb,
        const ::HIR::TraitImpl& impl
    ) const
{
    DEBUG("impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type << impl.m_params.fmt_bounds());
    return this->find_impl__check_crate_raw(
        sp,
        trait_path, trait_params, type,
        impl.m_params, impl.m_trait_args, impl.m_type,
        [&](auto impl_params, auto placeholders, auto match) {
            return found_cb( ImplRef(impl_params, trait_path, impl, mv$(placeholders)), (match == ::HIR::Compare::Fuzzy) );
        });
}

::HIR::Compare StaticTraitResolve::check_auto_trait_impl_destructure(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams* params_ptr, const ::HIR::TypeRef& type) const
{
    TRACE_FUNCTION_F("trait = " << trait << ", type = " << type);
    // HELPER: Search for an impl of this trait for an inner type, and return the match type
    auto type_impls_trait = [&](const auto& inner_ty) -> ::HIR::Compare {
        auto l_res = ::HIR::Compare::Unequal;
        this->find_impl(sp, trait, *params_ptr, inner_ty, [&](auto, auto is_fuzzy){ l_res = is_fuzzy ? ::HIR::Compare::Fuzzy : ::HIR::Compare::Equal; return !is_fuzzy; });
        DEBUG("[check_auto_trait_impl_destructure] " << inner_ty << " - " << l_res);
        return l_res;
        };

    // - If the type is a path (struct/enum/...), search for impls for all contained types.
    TU_IFLET( ::HIR::TypeRef::Data, type.m_data, Path, e,
        ::HIR::Compare  res = ::HIR::Compare::Equal;
        TU_MATCH( ::HIR::Path::Data, (e.path.m_data), (pe),
        (Generic,
            ::HIR::TypeRef  tmp;
            auto monomorph_cb = [&](const auto& gt)->const auto& {
                const auto& ge = gt.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    BUG(sp, "Self type in struct/enum generics");
                }
                else if( ge.binding >> 8 == 0 ) {
                    auto idx = ge.binding & 0xFF;
                    ASSERT_BUG(sp, idx < pe.m_params.m_types.size(), "Type parameter out of range - " << gt);
                    return pe.m_params.m_types[idx];
                }
                else {
                    BUG(sp, "Unexpected type parameter - " << gt << " in content of " << type);
                }
                };
            // HELPER: Get a possibily monomorphised version of the input type (stored in `tmp` if needed)
            auto monomorph_get = [&](const auto& ty)->const auto& {
                if( monomorphise_type_needed(ty) ) {
                    tmp = monomorphise_type_with(sp, ty,  monomorph_cb);
                    this->expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return ty;
                }
                };

            TU_MATCH( ::HIR::TypeRef::TypePathBinding, (e.binding), (tpb),
            (Opaque,
                BUG(sp, "Opaque binding on generic path - " << type);
                ),
            (Unbound,
                BUG(sp, "Unbound binding on generic path - " << type);
                ),
            (Struct,
                const auto& str = *tpb;

                // TODO: Somehow store a ruleset for auto traits on the type
                // - Map of trait->does_impl for local fields?
                // - Problems occur with type parameters
                TU_MATCH( ::HIR::Struct::Data, (str.m_data), (se),
                (Unit,
                    ),
                (Tuple,
                    for(const auto& fld : se)
                    {
                        const auto& fld_ty_mono = monomorph_get(fld.ent);
                        DEBUG("Struct::Tuple " << fld_ty_mono);
                        res &= type_impls_trait(fld_ty_mono);
                        if( res == ::HIR::Compare::Unequal )
                            return ::HIR::Compare::Unequal;
                    }
                    ),
                (Named,
                    for(const auto& fld : se)
                    {
                        const auto& fld_ty_mono = monomorph_get(fld.second.ent);
                        DEBUG("Struct::Named '" << fld.first << "' " << fld_ty_mono);

                        res &= type_impls_trait(fld_ty_mono);
                        if( res == ::HIR::Compare::Unequal )
                            return ::HIR::Compare::Unequal;
                    }
                    )
                )
                ),
            (Enum,
                const auto& enm = *tpb;

                for(const auto& var : enm.m_variants)
                {
                    TU_MATCH(::HIR::Enum::Variant, (var.second), (ve),
                    (Unit,
                        ),
                    (Value,
                        ),
                    (Tuple,
                        for(const auto& fld : ve)
                        {
                            const auto& fld_ty_mono = monomorph_get(fld.ent);
                            DEBUG("Enum '" << var.first << "'::Tuple " << fld_ty_mono);
                            res &= type_impls_trait(fld_ty_mono);
                            if( res == ::HIR::Compare::Unequal )
                                return ::HIR::Compare::Unequal;
                        }
                        ),
                    (Struct,
                        for(const auto& fld : ve)
                        {
                            const auto& fld_ty_mono = monomorph_get(fld.second.ent);
                            DEBUG("Enum '" << var.first << "'::Struct '" << fld.first << "' " << fld_ty_mono);

                            res &= type_impls_trait(fld_ty_mono);
                            if( res == ::HIR::Compare::Unequal )
                                return ::HIR::Compare::Unequal;
                        }
                        )
                    )
                }
                ),
            (Union,
                TODO(sp, "Check auto trait destructure on union " << type);
                )
            )
            DEBUG("- Nothing failed, calling callback");
            ),
        (UfcsUnknown,
            BUG(sp, "UfcsUnknown in typeck - " << type);
            ),
        (UfcsKnown,
            TODO(sp, "Check trait bounds for bound on UfcsKnown " << type);
            ),
        (UfcsInherent,
            TODO(sp, "Auto trait lookup on UFCS Inherent type");
            )
        )
        return res;
    )
    else TU_IFLET( ::HIR::TypeRef::Data, type.m_data, Tuple, e,
        ::HIR::Compare  res = ::HIR::Compare::Equal;
        for(const auto& sty : e)
        {
            res &= type_impls_trait(sty);
            if( res == ::HIR::Compare::Unequal )
                return ::HIR::Compare::Unequal;
        }
        return res;
    )
    else TU_IFLET( ::HIR::TypeRef::Data, type.m_data, Array, e,
        return type_impls_trait(*e.inner);
    )
    // Otherwise, there's no negative so it must be positive
    else {
        return ::HIR::Compare::Equal;
    }
}

void StaticTraitResolve::expand_associated_types(const Span& sp, ::HIR::TypeRef& input) const
{
    TRACE_FUNCTION_F(input);
    this->expand_associated_types_inner(sp, input);
}
void StaticTraitResolve::expand_associated_types_inner(const Span& sp, ::HIR::TypeRef& input) const
{
    TU_MATCH(::HIR::TypeRef::Data, (input.m_data), (e),
    (Infer,
        //if( m_treat_ivars_as_bugs ) {
        //    BUG(sp, "Encountered inferrence variable in static context");
        //}
        ),
    (Diverge,
        ),
    (Primitive,
        ),
    (Path,
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
        (Generic,
            for(auto& arg : e2.m_params.m_types)
                this->expand_associated_types_inner(sp, arg);
            ),
        (UfcsInherent,
            this->expand_associated_types_inner(sp, *e2.type);
            for(auto& arg : e2.params.m_types)
                this->expand_associated_types_inner(sp, arg);
            // TODO: impl params too?
            for(auto& arg : e2.impl_params.m_types)
                this->expand_associated_types_inner(sp, arg);
            ),
        (UfcsKnown,
            // - Only try resolving if the binding isn't known
            if( !e.binding.is_Unbound() )
                return ;
            this->expand_associated_types__UfcsKnown(sp, input);
            return;
            ),
        (UfcsUnknown,
            BUG(sp, "Encountered UfcsUnknown");
            )
        )
        ),
    (Generic,
        ),
    (TraitObject,
        // Recurse?
        ),
    (ErasedType,
        // Recurse?
        ),
    (Array,
        expand_associated_types_inner(sp, *e.inner);
        ),
    (Slice,
        expand_associated_types_inner(sp, *e.inner);
        ),
    (Tuple,
        for(auto& sub : e) {
            expand_associated_types_inner(sp, sub);
        }
        ),
    (Borrow,
        expand_associated_types_inner(sp, *e.inner);
        ),
    (Pointer,
        expand_associated_types_inner(sp, *e.inner);
        ),
    (Function,
        // Recurse?
        for(auto& ty : e.m_arg_types)
            expand_associated_types_inner(sp, ty);
        expand_associated_types_inner(sp, *e.m_rettype);
        ),
    (Closure,
        // Recurse?
        )
    )
}
void StaticTraitResolve::expand_associated_types__UfcsKnown(const Span& sp, ::HIR::TypeRef& input) const
{
    auto& e = input.m_data.as_Path();
    auto& e2 = e.path.m_data.as_UfcsKnown();

    this->expand_associated_types_inner(sp, *e2.type);
    for(auto& arg : e2.trait.m_params.m_types)
        this->expand_associated_types_inner(sp, arg);

    DEBUG("Locating associated type for " << e.path);

    // - If it's a closure, then the only trait impls are those generated by typeck
    TU_IFLET(::HIR::TypeRef::Data, e2.type->m_data, Closure, te,
        //if( te.node->m_obj_path == ::HIR::GenericPath() )
        //{
            const auto trait_fn = this->m_crate.get_lang_item_path(sp, "fn");
            const auto trait_fn_mut = this->m_crate.get_lang_item_path(sp, "fn_mut");
            const auto trait_fn_once = this->m_crate.get_lang_item_path(sp, "fn_once");
            if( e2.trait.m_path == trait_fn || e2.trait.m_path == trait_fn_mut || e2.trait.m_path == trait_fn_once  ) {
                if( e2.item == "Output" ) {
                    input = te.m_rettype->clone();
                    return ;
                }
                else {
                    ERROR(sp, E0000, "No associated type " << e2.item << " for trait " << e2.trait);
                }
            }
        //}
        //else
        //{
        //    // TODO: Locate impl _without_ binding params too hard?
        //}
    )

    // If it's a TraitObject, then maybe we're asking for a bound
    TU_IFLET(::HIR::TypeRef::Data, e2.type->m_data, TraitObject, te,
        const auto& data_trait = te.m_trait.m_path;
        if( e2.trait.m_path == data_trait.m_path ) {
            if( e2.trait.m_params == data_trait.m_params )
            {
                auto it = te.m_trait.m_type_bounds.find( e2.item );
                if( it == te.m_trait.m_type_bounds.end() ) {
                    // TODO: Mark as opaque and return.
                    // - Why opaque? It's not bounded, don't even bother
                    TODO(sp, "Handle unconstrained associate type " << e2.item << " from " << *e2.type);
                }

                input = it->second.clone();
                return ;
            }
        }
    )

    // 1. Bounds
    bool rv;
    bool assume_opaque = true;
    rv = this->iterate_bounds([&](const auto& b) {
        TU_MATCH_DEF(::HIR::GenericBound, (b), (be),
        (
            ),
        (TraitBound,
            DEBUG("Trait bound - " << be.type << " : " << be.trait);
            // 1. Check if the type matches
            //  - TODO: This should be a fuzzier match?
            if( be.type != *e2.type )
                return false;
            // 2. Check if the trait (or any supertrait) includes e2.trait
            if( be.trait.m_path == e2.trait ) {
                auto it = be.trait.m_type_bounds.find(e2.item);
                // 1. Check if the bounds include the desired item
                if( it == be.trait.m_type_bounds.end() ) {
                    // If not, assume it's opaque and return as such
                    // TODO: What happens if there's two bounds that overlap? 'F: FnMut<()>, F: FnOnce<(), Output=Bar>'
                    DEBUG("Found impl for " << input << " but no bound on item, assuming opaque");
                }
                else {
                    assume_opaque = false;
                    input = it->second.clone();
                }
                return true;
            }

            bool found_supertrait = this->find_named_trait_in_trait(sp,
                e2.trait.m_path, e2.trait.m_params,
                *be.trait.m_trait_ptr, be.trait.m_path.m_path, be.trait.m_path.m_params, *e2.type,
                [&e2,&input,&assume_opaque](const auto& params, auto assoc){
                    auto it = assoc.find(e2.item);
                    if( it != assoc.end() ) {
                        assume_opaque = false;
                        DEBUG("Found associated type " << input << " = " << it->second);
                        input = it->second.clone();
                    }
                    return true;
                }
                );
            if( found_supertrait ) {
                auto it = be.trait.m_type_bounds.find(e2.item);
                // 1. Check if the bounds include the desired item
                if( it == be.trait.m_type_bounds.end() ) {
                    // If not, assume it's opaque and return as such
                    // TODO: What happens if there's two bounds that overlap? 'F: FnMut<()>, F: FnOnce<(), Output=Bar>'
                    if( assume_opaque )
                        DEBUG("Found impl for " << input << " but no bound on item, assuming opaque");
                }
                else {
                    assume_opaque = false;
                    input = it->second.clone();
                }
                return true;
            }

            // - Didn't match
            ),
        (TypeEquality,
            DEBUG("Equality - " << be.type << " = " << be.other_type);
            if( input == be.type ) {
                input = be.other_type.clone();
                return true;
            }
            )
        )
        return false;
        });
    if( rv ) {
        if( assume_opaque ) {
            input.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
            DEBUG("Assuming that " << input << " is an opaque name");

            this->replace_equalities(input);
        }
        this->expand_associated_types_inner(sp, input);
        return;
    }

    // If the type of this UfcsKnown is ALSO a UfcsKnown - Check if it's bounded by this trait with equality
    // Use bounds on other associated types too (if `e2.type` was resolved to a fixed associated type)
    TU_IFLET(::HIR::TypeRef::Data, e2.type->m_data, Path, te_inner,
        TU_IFLET(::HIR::Path::Data, te_inner.path.m_data, UfcsKnown, pe_inner,
            // TODO: Search for equality bounds on this associated type (e3) that match the entire type (e2)
            // - Does simplification of complex associated types
            const auto& trait_ptr = this->m_crate.get_trait_by_path(sp, pe_inner.trait.m_path);
            const auto& assoc_ty = trait_ptr.m_types.at(pe_inner.item);

            // Resolve where Self=pe_inner.type (i.e. for the trait this inner UFCS is on)
            auto cb_placeholders_trait = [&](const auto& ty)->const auto&{
                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Generic, e,
                    if( e.binding == 0xFFFF )
                        return *pe_inner.type;
                    else {
                        // TODO: Look in pe_inner.trait.m_params
                        TODO(sp, "Handle type params when expanding associated bound (#" << e.binding << " " << e.name);
                    }
                )
                else {
                    return ty;
                }
                };
            for(const auto& bound : assoc_ty.m_trait_bounds)
            {
                // If the bound is for Self and the outer trait
                // - TODO: Parameters?
                if( bound.m_path == e2.trait ) {
                    auto it = bound.m_type_bounds.find( e2.item );
                    if( it != bound.m_type_bounds.end() ) {
                        if( monomorphise_type_needed(it->second) ) {
                            input = monomorphise_type_with(sp, it->second, cb_placeholders_trait);
                        }
                        else {
                            input = it->second.clone();
                        }
                        this->expand_associated_types(sp, input);
                        return ;
                    }
                }
            }
            DEBUG("e2 = " << *e2.type << ", input = " << input);
        )
    )

    // 2. Crate-level impls

    // - Search for the actual trait containing this associated type
    ::HIR::GenericPath  trait_path;
    if( !this->trait_contains_type(sp, e2.trait, this->m_crate.get_trait_by_path(sp, e2.trait.m_path), e2.item, trait_path) )
        BUG(sp, "Cannot find associated type " << e2.item << " anywhere in trait " << e2.trait);
    //e2.trait = mv$(trait_path);

    ::ImplRef  best_impl;
    rv = this->find_impl(sp, trait_path.m_path, trait_path.m_params, *e2.type, [&](auto impl, bool fuzzy) {
        DEBUG("[expand_associated_types] Found " << impl);
        // If a fuzzy match was found, monomorphise and EAT the checked types and try again
        // - A fuzzy can be caused by an opaque match.
        // - TODO: Move this logic into `find_impl`
        if( fuzzy ) {
            DEBUG("[expand_associated_types] - Fuzzy, monomorph+expand and recheck");

            auto impl_ty = impl.get_impl_type();
            this->expand_associated_types(sp, impl_ty);
            if(impl_ty != *e2.type) {
                DEBUG("[expand_associated_types] - Fuzzy - Doesn't match");
                return false;
            }
            auto pp = impl.get_trait_params();
            for(auto& ty : pp.m_types)
                this->expand_associated_types(sp, ty);
            if( pp != trait_path.m_params ) {
                DEBUG("[expand_associated_types] - Fuzzy - Doesn't match");
                return false;
            }
            DEBUG("[expand_associated_types] - Fuzzy - Actually matches");
        }

        if( impl.type_is_specialisable(e2.item.c_str()) ) {
            if( impl.more_specific_than(best_impl) ) {
                best_impl = mv$(impl);
                DEBUG("- Still specialisable");
            }
            return false;
        }
        else {
            auto nt = impl.get_type( e2.item.c_str() );
            if( nt == ::HIR::TypeRef() ) {
                DEBUG("Mark  " << e.path << " as opaque");
                e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                this->replace_equalities(input);
            }
            else {
                DEBUG("Converted UfcsKnown - " << e.path << " = " << nt);
                input = mv$(nt);
            }
            return true;
        }
        });
    if( rv ) {
        this->expand_associated_types(sp, input);
        return;
    }
    if( best_impl.is_valid() ) {
        e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
        this->replace_equalities(input);
        DEBUG("- Couldn't find a non-specialised impl of " << trait_path << " for " << *e2.type << " - treating as opaque");
        return ;
    }

    ERROR(sp, E0000, "Cannot find an implementation of " << trait_path << " for " << *e2.type);
}

void StaticTraitResolve::replace_equalities(::HIR::TypeRef& input) const
{
    TRACE_FUNCTION_F("input="<<input);
    DEBUG("m_type_equalities = {" << m_type_equalities << "}");
    // - Check if there's an alias for this opaque name
    auto a = m_type_equalities.find(input);
    if( a != m_type_equalities.end() ) {
        input = a->second.clone();
        DEBUG("- Replace with " << input);
    }
}

// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
bool StaticTraitResolve::iterate_bounds( ::std::function<bool(const ::HIR::GenericBound&)> cb) const
{
    const ::HIR::GenericParams* v[2] = { m_item_generics, m_impl_generics };
    for(auto p : v)
    {
        if( !p )    continue ;
        for(const auto& b : p->m_bounds)
            if(cb(b))   return true;
    }
    return false;
}
// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
bool StaticTraitResolve::find_named_trait_in_trait(const Span& sp,
        const ::HIR::SimplePath& des, const ::HIR::PathParams& des_params,
        const ::HIR::Trait& trait_ptr, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& pp,
        const ::HIR::TypeRef& target_type,
        ::std::function<void(const ::HIR::PathParams&, ::std::map< ::std::string, ::HIR::TypeRef>)> callback
    ) const
{
    TRACE_FUNCTION_F(des << des_params << " from " << trait_path << pp);
    if( pp.m_types.size() != trait_ptr.m_params.m_types.size() ) {
        BUG(sp, "Incorrect number of parameters for trait - " << trait_path << pp);
    }

    auto monomorph_cb = [&](const auto& gt)->const auto& {
        const auto& ge = gt.m_data.as_Generic();
        if( ge.binding == 0xFFFF ) {
            return target_type;
        }
        else {
            if( ge.binding >= pp.m_types.size() )
                BUG(sp, "find_named_trait_in_trait - Generic #" << ge.binding << " " << ge.name << " out of range");
            return pp.m_types[ge.binding];
        }
        };

    for( const auto& pt : trait_ptr.m_all_parent_traits )
    {
        auto pt_mono = monomorphise_traitpath_with(sp, pt, monomorph_cb, false);

        DEBUG(pt << " => " << pt_mono);
        if( pt.m_path.m_path == des && pt_mono.m_path.m_params == des_params )
        {
            callback( pt_mono.m_path.m_params, mv$(pt_mono.m_type_bounds) );
            return true;
        }
    }

    return false;
}
bool StaticTraitResolve::trait_contains_type(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::std::string& name,  ::HIR::GenericPath& out_path) const
{
    TRACE_FUNCTION_FR("name="<<name << ", trait=" << trait_path, out_path);
    auto it = trait_ptr.m_types.find(name);
    if( it != trait_ptr.m_types.end() ) {
        out_path = trait_path.clone();
        return true;
    }

    auto monomorph = [&](const auto& gt)->const auto& {
            const auto& ge = gt.m_data.as_Generic();
            assert(ge.binding < 256);
            assert(ge.binding < trait_path.m_params.m_types.size());
            return trait_path.m_params.m_types[ge.binding];
            };
    for(const auto& st : trait_ptr.m_all_parent_traits)
    {
        if( st.m_trait_ptr->m_types.count(name) )
        {
            out_path.m_path = st.m_path.m_path;
            out_path.m_params = monomorphise_path_params_with(sp, st.m_path.m_params, monomorph, false);
            return true;
        }
    }
    return false;
}

bool StaticTraitResolve::type_is_copy(const Span& sp, const ::HIR::TypeRef& ty) const
{
    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
    (Generic,
        {
            auto it = m_copy_cache.find(ty);
            if( it != m_copy_cache.end() )
            {
                return it->second;
            }
        }
        bool rv = this->iterate_bounds([&](const auto& b) {
            auto pp = ::HIR::PathParams();
            return this->find_impl__check_bound(sp, m_lang_Copy, &pp, ty, [&](auto , bool ){ return true; },  b);
            });
        m_copy_cache.insert(::std::make_pair( ty.clone(), rv ));
        return rv;
        ),
    (Path,
        {
            auto it = m_copy_cache.find(ty);
            if( it != m_copy_cache.end() )
                return it->second;
        }
        auto pp = ::HIR::PathParams();
        bool rv = this->find_impl(sp, m_lang_Copy, &pp, ty, [&](auto , bool){ return true; }, true);
        m_copy_cache.insert(::std::make_pair( ty.clone(), rv ));
        return rv;
        ),
    (Diverge,
        // The ! type is kinda Copy ...
        return true;
        ),
    (Closure,
        return false;
        ),
    (Infer,
        // Shouldn't be hit
        return false;
        ),
    (Borrow,
        // Only shared &-ptrs are copy
        return (e.type == ::HIR::BorrowType::Shared);
        ),
    (Pointer,
        // All raw pointers are Copy
        return true;
        ),
    (Function,
        // All function pointers are Copy
        return true;
        ),
    (Primitive,
        // All primitives (except the unsized `str`) are Copy
        return e != ::HIR::CoreType::Str;
        ),
    (Array,
        return e.size_val == 0 || type_is_copy(sp, *e.inner);
        ),
    (Slice,
        // [T] isn't Sized, so isn't Copy ether
        return false;
        ),
    (TraitObject,
        // (Trait) isn't Sized, so isn't Copy ether
        return false;
        ),
    (ErasedType,
        for(const auto& trait : e.m_traits)
        {
            if( find_named_trait_in_trait(sp, m_lang_Copy, {},  *trait.m_trait_ptr, trait.m_path.m_path, trait.m_path.m_params,  ty, [](const auto&, auto ){ }) ) {
                return true;
            }
        }
        return false;
        ),
    (Tuple,
        for(const auto& ty : e)
            if( !type_is_copy(sp, ty) )
                return false;
        return true;
        )
    )
    throw "";
}

bool StaticTraitResolve::type_is_sized(const Span& sp, const ::HIR::TypeRef& ty) const
{
    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
    (Generic,
        if( e.binding == 0xFFFF ) {
            // TODO: Self: Sized?
            return true;
        }
        else if( (e.binding >> 8) == 0 ) {
            auto idx = e.binding & 0xFF;
            assert( m_impl_generics );
            assert( idx < m_impl_generics->m_types.size() );
            return m_impl_generics->m_types[idx].m_is_sized;
        }
        else if( (e.binding >> 8) == 1 ) {
            auto idx = e.binding & 0xFF;
            assert( m_item_generics );
            assert( idx < m_item_generics->m_types.size() );
            return m_item_generics->m_types[idx].m_is_sized;
        }
        else {
            BUG(sp, "");
        }
        ),
    (Path,
        auto pp = ::HIR::PathParams();
        // TODO: Destructure?
        return true;
        //return this->find_impl(sp, m_lang_Sized, &pp, ty, [&](auto , bool){ return true; }, true);
        ),
    (Diverge,
        // The ! type is kinda Copy ...
        return true;
        ),
    (Closure,
        return true;
        ),
    (Infer,
        // Shouldn't be hit
        return false;
        ),
    (Borrow,
        return true;
        ),
    (Pointer,
        return true;
        ),
    (Function,
        return true;
        ),
    (Primitive,
        // All primitives (except the unsized `str`) are Sized
        return e != ::HIR::CoreType::Str;
        ),
    (Array,
        return true;
        ),
    (Slice,
        return false;
        ),
    (TraitObject,
        return false;
        ),
    (ErasedType,
        // NOTE: All erased types are implicitly Sized
        return true;
        ),
    (Tuple,
        for(const auto& ty : e)
            if( !type_is_sized(sp, ty) )
                return false;
        return true;
        )
    )
    throw "";
}

const ::HIR::TypeRef* StaticTraitResolve::is_type_owned_box(const ::HIR::TypeRef& ty) const
{
    if( ! ty.m_data.is_Path() ) {
        return nullptr;
    }
    const auto& te = ty.m_data.as_Path();

    if( ! te.path.m_data.is_Generic() ) {
        return nullptr;
    }
    const auto& pe = te.path.m_data.as_Generic();

    if( pe.m_path != m_lang_Box ) {
        return nullptr;
    }
    // TODO: Properly assert?
    return &pe.m_params.m_types.at(0);
}
const ::HIR::TypeRef* StaticTraitResolve::is_type_phantom_data(const ::HIR::TypeRef& ty) const
{
    if( ! ty.m_data.is_Path() ) {
        return nullptr;
    }
    const auto& te = ty.m_data.as_Path();

    if( ! te.path.m_data.is_Generic() ) {
        return nullptr;
    }
    const auto& pe = te.path.m_data.as_Generic();

    if( pe.m_path != m_lang_PhantomData ) {
        return nullptr;
    }
    // TODO: Properly assert?
    return &pe.m_params.m_types.at(0);
}
