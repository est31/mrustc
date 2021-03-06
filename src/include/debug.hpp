/*
 */
#pragma once
#include <sstream>
#include <cassert>
#include <functional>

extern int g_debug_indent_level;

#ifndef DISABLE_DEBUG
# define INDENT()    do { g_debug_indent_level += 1; assert(g_debug_indent_level<300); } while(0)
# define UNINDENT()    do { g_debug_indent_level -= 1; } while(0)
# define DEBUG(ss)   do{ if(debug_enabled()) { debug_output(g_debug_indent_level, __FUNCTION__) << ss << ::std::endl; } } while(0)
# define TRACE_FUNCTION  TraceLog _tf_(__func__)
# define TRACE_FUNCTION_F(ss)    TraceLog _tf_(__func__, [&](::std::ostream&__os){ __os << ss; })
# define TRACE_FUNCTION_FR(ss,ss2)    TraceLog _tf_(__func__, [&](::std::ostream&__os){ __os << ss; }, [&](::std::ostream&__os){ __os << ss2;})
#else
# define INDENT()    do { } while(0)
# define UNINDENT()    do {} while(0)
# define DEBUG(ss)   do{ if(false) (void)(::NullSink() << ss); } while(0)
# define TRACE_FUNCTION  do{} while(0)
# define TRACE_FUNCTION_F(ss)  do{ if(false) (void)(::NullSink() << ss); } while(0)
# define TRACE_FUNCTION_FR(ss,ss2)  do{ if(false) (void)(::NullSink() << ss); if(false) (void)(::NullSink() << ss2); } while(0)
#endif

extern bool debug_enabled();
extern ::std::ostream& debug_output(int indent, const char* function);

struct RepeatLitStr
{
    const char *s;
    int n;

    friend ::std::ostream& operator<<(::std::ostream& os, const RepeatLitStr& r) {
        for(int i = 0; i < r.n; i ++ )
            os << r.s;
        return os;
    }
};

class NullSink:
    public ::std::ostream
{
public:
    NullSink() {}
};

class TraceLog
{
    const char* m_tag;
    ::std::function<void(::std::ostream&)>  m_ret;
public:
    TraceLog(const char* tag, ::std::function<void(::std::ostream&)> info_cb, ::std::function<void(::std::ostream&)> ret);
    TraceLog(const char* tag, ::std::function<void(::std::ostream&)> info_cb);
    TraceLog(const char* tag);
    ~TraceLog();
};

struct FmtLambda
{
    ::std::function<void(::std::ostream&)>  m_cb;
    friend ::std::ostream& operator<<(::std::ostream& os, const FmtLambda& x) {
        x.m_cb(os);
        return os;
    }
};
#define FMT_CB(os, ...)  ::FmtLambda { [&](auto& os) { __VA_ARGS__ } }


