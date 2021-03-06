/*
 */
#pragma once

#include <cassert>

class TypeRef;
class TokenTree;
namespace AST {
    class Pattern;
    class Path;
    class ExprNode;
    class MetaItem;
    template<typename T> class Named;
    class Item;
};


class InterpolatedFragment
{
public:
    enum Type
    {
        TT,
        PAT,
        PATH,
        TYPE,

        EXPR,
        STMT,
        BLOCK,

        META,
        ITEM,
    } m_type;

    // Owned type-pruned pointer
    void*   m_ptr;

    InterpolatedFragment(InterpolatedFragment&& );
    InterpolatedFragment& operator=(InterpolatedFragment&& );
    //InterpolatedFragment(const InterpolatedFragment& );
    InterpolatedFragment(TokenTree );
    InterpolatedFragment(::AST::Pattern);
    InterpolatedFragment(::AST::Path);
    InterpolatedFragment(::TypeRef);
    InterpolatedFragment(::AST::MetaItem );
    InterpolatedFragment(::AST::Named<AST::Item> );
    ~InterpolatedFragment();
    InterpolatedFragment(Type , ::AST::ExprNode*);

    TokenTree& as_tt() { assert(m_type == TT); return *reinterpret_cast<TokenTree*>(m_ptr); }
    const TokenTree& as_tt() const { assert(m_type == TT); return *reinterpret_cast<TokenTree*>(m_ptr); }

    friend ::std::ostream& operator<<(::std::ostream& os, const InterpolatedFragment& x);
};
