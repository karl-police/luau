// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "ldo.h"

#include "lstring.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lvm.h"

#if LUA_USE_LONGJMP
#include <setjmp.h>
#include <stdlib.h>
#else
#include <stdexcept>
#endif

#include <string.h>

LUAU_DYNAMIC_FASTFLAGVARIABLE(LuauErrorYield, false)

// keep max stack allocation request under 1GB
#define MAX_STACK_SIZE (int(1024 / sizeof(TValue)) * 1024 * 1024)

/*
** {======================================================
** Error-recovery functions
** =======================================================
*/

#if LUA_USE_LONGJMP
struct lua_jmpbuf
{
    lua_jmpbuf* volatile prev;
    volatile int status;
    jmp_buf buf;
};

// use POSIX versions of setjmp/longjmp if possible: they don't save/restore signal mask and are therefore faster
#if defined(__linux__) || defined(__APPLE__)
#define LUAU_SETJMP(buf) _setjmp(buf)
#define LUAU_LONGJMP(buf, code) _longjmp(buf, code)
#else
#define LUAU_SETJMP(buf) setjmp(buf)
#define LUAU_LONGJMP(buf, code) longjmp(buf, code)
#endif

int luaD_rawrunprotected(lua_State* L, Pfunc f, void* ud)
{
    lua_jmpbuf jb;
    jb.prev = L->global->errorjmp;
    jb.status = 0;
    L->global->errorjmp = &jb;

    if (LUAU_SETJMP(jb.buf) == 0)
        f(L, ud);

    L->global->errorjmp = jb.prev;
    return jb.status;
}

l_noret luaD_throw(lua_State* L, int errcode)
{
    if (lua_jmpbuf* jb = L->global->errorjmp)
    {
        jb->status = errcode;
        LUAU_LONGJMP(jb->buf, 1);
    }

    if (L->global->cb.panic)
        L->global->cb.panic(L, errcode);

    abort();
}
#else
class lua_exception : public std::exception
{
public:
    lua_exception(lua_State* L, int status)
        : L(L)
        , status(status)
    {
    }

    const char* what() const throw() override
    {
        // LUA_ERRRUN passes error object on the stack
        if (status == LUA_ERRRUN)
            if (const char* str = lua_tostring(L, -1))
                return str;

        switch (status)
        {
        case LUA_ERRRUN:
            return "lua_exception: runtime error";
        case LUA_ERRSYNTAX:
            return "lua_exception: syntax error";
        case LUA_ERRMEM:
            return "lua_exception: " LUA_MEMERRMSG;
        case LUA_ERRERR:
            return "lua_exception: " LUA_ERRERRMSG;
        default:
            return "lua_exception: unexpected exception status";
        }
    }

    int getStatus() const
    {
        return status;
    }

    const lua_State* getThread() const
    {
        return L;
    }

private:
    lua_State* L;
    int status;
};

int luaD_rawrunprotected(lua_State* L, Pfunc f, void* ud)
{
    int status = 0;

    try
    {
        f(L, ud);
        return 0;
    }
    catch (lua_exception& e)
    {
        // It is assumed/required that the exception caught here was thrown from the same Luau state.
        // If this assert fires, it indicates a lua_exception was not properly caught and propagated
        // to the exception handler for a different Luau state. Report this issue to the Luau team if
        // you need more information or assistance resolving this assert.
        LUAU_ASSERT(e.getThread() == L);

        status = e.getStatus();
    }
    catch (std::exception& e)
    {
        // Luau will never throw this, but this can catch exceptions that escape from C++ implementations of external functions
        try
        {
            // there's no exception object on stack; let's push the error on stack so that error handling below can proceed
            luaG_pusherror(L, e.what());
            status = LUA_ERRRUN;
        }
        catch (std::exception&)
        {
            // out of memory while allocating error string
            status = LUA_ERRMEM;
        }
    }

    return status;
}

l_noret luaD_throw(lua_State* L, int errcode)
{
    throw lua_exception(L, errcode);
}
#endif

// }======================================================

static void correctstack(lua_State* L, TValue* oldstack)
{
    L->top = (L->top - oldstack) + L->stack;
    for (UpVal* up = L->openupval; up != NULL; up = up->u.open.threadnext)
        up->v = (up->v - oldstack) + L->stack;
    for (CallInfo* ci = L->base_ci; ci <= L->ci; ci++)
    {
        ci->top = (ci->top - oldstack) + L->stack;
        ci->base = (ci->base - oldstack) + L->stack;
        ci->func = (ci->func - oldstack) + L->stack;
    }
    L->base = (L->base - oldstack) + L->stack;
}

void luaD_reallocstack(lua_State* L, int newsize, int fornewci)
{
    // throw 'out of memory' error because space for a custom error message cannot be guaranteed here
    if (newsize > MAX_STACK_SIZE)
    {
        // reallocation was performed to setup a new CallInfo frame, which we have to remove
        if (fornewci)
        {
            CallInfo* cip = L->ci - 1;

            L->ci = cip;
            L->base = cip->base;
            L->top = cip->top;
        }

        luaD_throw(L, LUA_ERRMEM);
    }

    TValue* oldstack = L->stack;
    int realsize = newsize + EXTRA_STACK;
    LUAU_ASSERT(L->stack_last - L->stack == L->stacksize - EXTRA_STACK);
    luaM_reallocarray(L, L->stack, L->stacksize, realsize, TValue, L->memcat);
    TValue* newstack = L->stack;
    for (int i = L->stacksize; i < realsize; i++)
        setnilvalue(newstack + i); // erase new segment
    L->stacksize = realsize;
    L->stack_last = newstack + newsize;
    correctstack(L, oldstack);
}

void luaD_reallocCI(lua_State* L, int newsize)
{
    CallInfo* oldci = L->base_ci;
    luaM_reallocarray(L, L->base_ci, L->size_ci, newsize, CallInfo, L->memcat);
    L->size_ci = newsize;
    L->ci = (L->ci - oldci) + L->base_ci;
    L->end_ci = L->base_ci + L->size_ci - 1;
}

void luaD_growstack(lua_State* L, int n)
{
    luaD_reallocstack(L, getgrownstacksize(L, n), 0);
}

CallInfo* luaD_growCI(lua_State* L)
{
    // allow extra stack space to handle stack overflow in xpcall
    const int hardlimit = LUAI_MAXCALLS + (LUAI_MAXCALLS >> 3);

    if (L->size_ci >= hardlimit)
        luaD_throw(L, LUA_ERRERR); // error while handling stack error

    int request = L->size_ci * 2;
    luaD_reallocCI(L, L->size_ci >= LUAI_MAXCALLS ? hardlimit : request < LUAI_MAXCALLS ? request : LUAI_MAXCALLS);

    if (L->size_ci > LUAI_MAXCALLS)
        luaG_runerror(L, "stack overflow");

    return ++L->ci;
}

void luaD_checkCstack(lua_State* L)
{
    // allow extra stack space to handle stack overflow in xpcall
    const int hardlimit = LUAI_MAXCCALLS + (LUAI_MAXCCALLS >> 3);

    if (L->nCcalls == LUAI_MAXCCALLS)
        luaG_runerror(L, "C stack overflow");
    else if (L->nCcalls >= hardlimit)
        luaD_throw(L, LUA_ERRERR); // error while handling stack error
}

static void performcall(lua_State* L, StkId func, int nresults)
{
    if (luau_precall(L, func, nresults) == PCRLUA)
    {                                        // is a Lua function?
        L->ci->flags |= LUA_CALLINFO_RETURN; // luau_execute will stop after returning from the stack frame

        bool oldactive = L->isactive;
        L->isactive = true;
        luaC_threadbarrier(L);

        luau_execute(L); // call it

        if (!oldactive)
            L->isactive = false;
    }
}

/*
** Call a function (C or Lua). The function to be called is at *func.
** The arguments are on the stack, right after the function.
** When returns, all the results are on the stack, starting at the original
** function position.
*/
void luaD_call(lua_State* L, StkId func, int nresults)
{
    if (++L->nCcalls >= LUAI_MAXCCALLS)
        luaD_checkCstack(L);

    // when called from a yieldable C function, maintain yieldable invariant (baseCcalls <= nCcalls)
    bool fromyieldableccall = false;

    if (L->ci != L->base_ci)
    {
        Closure* ccl = clvalue(L->ci->func);

        if (ccl->isC && ccl->c.cont)
        {
            fromyieldableccall = true;
            L->baseCcalls++;
        }
    }

    ptrdiff_t funcoffset = savestack(L, func);
    ptrdiff_t cioffset = saveci(L, L->ci);

    if (DFFlag::LuauErrorYield)
    {
        performcall(L, func, nresults);
    }
    else
    {
        if (luau_precall(L, func, nresults) == PCRLUA)
        {                                        // is a Lua function?
            L->ci->flags |= LUA_CALLINFO_RETURN; // luau_execute will stop after returning from the stack frame

            bool oldactive = L->isactive;
            L->isactive = true;
            luaC_threadbarrier(L);

            luau_execute(L); // call it

            if (!oldactive)
                L->isactive = false;
        }
    }

    bool yielded = L->status == LUA_YIELD || L->status == LUA_BREAK;

    if (fromyieldableccall)
    {
        // restore original yieldable invariant
        // in case of an error, this would either be restored by luaD_pcall or the thread would no longer be resumable
        L->baseCcalls--;

        // on yield, we have to set the CallInfo top of the C function including slots for expected results, to restore later
        if (yielded)
        {
            CallInfo* callerci = restoreci(L, cioffset);
            callerci->top = restorestack(L, funcoffset) + (nresults != LUA_MULTRET ? nresults : 0);
        }
    }

    if (nresults != LUA_MULTRET && !yielded)
        L->top = restorestack(L, funcoffset) + nresults;

    L->nCcalls--;
    luaC_checkGC(L);
}

// Non-yieldable version of luaD_call, used primarily to call an error handler which cannot yield
void luaD_callny(lua_State* L, StkId func, int nresults)
{
    if (++L->nCcalls >= LUAI_MAXCCALLS)
        luaD_checkCstack(L);

    LUAU_ASSERT(L->nCcalls > L->baseCcalls);

    ptrdiff_t funcoffset = savestack(L, func);

    performcall(L, func, nresults);

    LUAU_ASSERT(L->status != LUA_YIELD && L->status != LUA_BREAK);

    if (nresults != LUA_MULTRET)
        L->top = restorestack(L, funcoffset) + nresults;

    L->nCcalls--;
    luaC_checkGC(L);
}

static void seterrorobj(lua_State* L, int errcode, StkId oldtop)
{
    switch (errcode)
    {
    case LUA_ERRMEM:
    {
        setsvalue(L, oldtop, luaS_newliteral(L, LUA_MEMERRMSG)); // can not fail because string is pinned in luaopen
        break;
    }
    case LUA_ERRERR:
    {
        setsvalue(L, oldtop, luaS_newliteral(L, LUA_ERRERRMSG)); // can not fail because string is pinned in luaopen
        break;
    }
    case LUA_ERRSYNTAX:
    case LUA_ERRRUN:
    {
        setobj2s(L, oldtop, L->top - 1); // error message on current top
        break;
    }
    }
    L->top = oldtop + 1;
}

static void resume_continue(lua_State* L)
{
    // unroll Lua/C combined stack, processing continuations
    while (L->status == 0 && L->ci > L->base_ci)
    {
        LUAU_ASSERT(L->baseCcalls == L->nCcalls);

        Closure* cl = curr_func(L);

        if (cl->isC)
        {
            LUAU_ASSERT(cl->c.cont);

            // C continuation; we expect this to be followed by Lua continuations
            int n = cl->c.cont(L, 0);

            // continuation can break or yield again
            if (L->status == LUA_BREAK || L->status == LUA_YIELD)
                break;

            luau_poscall(L, L->top - n);
        }
        else
        {
            // Lua continuation; it terminates at the end of the stack or at another C continuation
            luau_execute(L);
        }
    }
}

static void resume(lua_State* L, void* ud)
{
    StkId firstArg = cast_to(StkId, ud);

    if (L->status == 0)
    {
        // start coroutine
        LUAU_ASSERT(L->ci == L->base_ci && firstArg >= L->base);
        if (firstArg == L->base)
            luaG_runerror(L, "cannot resume dead coroutine");

        if (luau_precall(L, firstArg - 1, LUA_MULTRET) != PCRLUA)
            return;

        L->ci->flags |= LUA_CALLINFO_RETURN;
    }
    else
    {
        // resume from previous yield or break
        LUAU_ASSERT(firstArg >= L->base);
        LUAU_ASSERT(L->status == LUA_YIELD || L->status == LUA_BREAK);
        L->status = 0;

        Closure* cl = curr_func(L);

        if (cl->isC)
        {
            // if the top stack frame is a C call continuation, resume_continue will handle that case
            if (!cl->c.cont)
            {
                // finish interrupted execution of `OP_CALL'
                luau_poscall(L, firstArg);
            }
            else
            {
                // restore arguments we have protected for C continuation
                L->base = L->ci->base;
            }
        }
        else
        {
            // yielded inside a hook: just continue its execution
            L->base = L->ci->base;
        }
    }

    // run continuations from the stack; typically resumes Lua code and pcalls
    resume_continue(L);
}

static CallInfo* resume_findhandler(lua_State* L)
{
    CallInfo* ci = L->ci;

    while (ci > L->base_ci)
    {
        if (ci->flags & LUA_CALLINFO_HANDLE)
            return ci;

        ci--;
    }

    return NULL;
}

static void resume_handle(lua_State* L, void* ud)
{
    CallInfo* ci = (CallInfo*)ud;
    Closure* cl = ci_func(ci);

    LUAU_ASSERT(ci->flags & LUA_CALLINFO_HANDLE);
    LUAU_ASSERT(cl->isC && cl->c.cont);
    LUAU_ASSERT(L->status != 0);

    // restore nCcalls back to base since this might not have happened during error handling
    L->nCcalls = L->baseCcalls;

    // make sure we don't run the handler the second time
    ci->flags &= ~LUA_CALLINFO_HANDLE;

    // restore thread status to 0 since we're handling the error
    int status = L->status;
    L->status = 0;

    // push error object to stack top if it's not already there
    if (status != LUA_ERRRUN)
        seterrorobj(L, status, L->top);

    // adjust the stack frame for ci to prepare for cont call
    L->base = ci->base;
    ci->top = L->top;

    // save ci pointer - it will be invalidated by cont call!
    ptrdiff_t old_ci = saveci(L, ci);

    // handle the error in continuation; note that this executes on top of original stack!
    int n = cl->c.cont(L, status);

    // restore the stack frame to the frame with continuation
    L->ci = restoreci(L, old_ci);

    // close eventual pending closures; this means it's now safe to restore stack
    luaF_close(L, L->ci->base);

    // finish cont call and restore stack to previous ci top
    luau_poscall(L, L->top - n);

    // run remaining continuations from the stack; typically resumes pcalls
    resume_continue(L);
}

static int resume_error(lua_State* L, const char* msg, int narg)
{
    L->top -= narg;
    setsvalue(L, L->top, luaS_new(L, msg));
    incr_top(L);
    return LUA_ERRRUN;
}

static void resume_finish(lua_State* L, int status)
{
    L->nCcalls = L->baseCcalls;
    L->isactive = false;

    if (status != 0)
    {                                  // error?
        L->status = cast_byte(status); // mark thread as `dead'
        seterrorobj(L, status, L->top);
        L->ci->top = L->top;
    }
    else if (L->status == 0)
    {
        expandstacklimit(L, L->top);
    }
}

int lua_resume(lua_State* L, lua_State* from, int nargs)
{
    api_check(L, L->top - L->base >= nargs);

    int status;
    if (L->status != LUA_YIELD && L->status != LUA_BREAK && (L->status != 0 || L->ci != L->base_ci))
        return resume_error(L, "cannot resume non-suspended coroutine", nargs);

    L->nCcalls = from ? from->nCcalls : 0;
    if (L->nCcalls >= LUAI_MAXCCALLS)
        return resume_error(L, "C stack overflow", nargs);

    L->baseCcalls = ++L->nCcalls;
    L->isactive = true;

    luaC_threadbarrier(L);

    status = luaD_rawrunprotected(L, resume, L->top - nargs);

    CallInfo* ch = NULL;
    while (status != 0 && (ch = resume_findhandler(L)) != NULL)
    {
        L->status = cast_byte(status);
        status = luaD_rawrunprotected(L, resume_handle, ch);
    }

    resume_finish(L, status);
    --L->nCcalls;
    return L->status;
}

int lua_resumeerror(lua_State* L, lua_State* from)
{
    api_check(L, L->top - L->base >= 1);

    int status;
    if (L->status != LUA_YIELD && L->status != LUA_BREAK && (L->status != 0 || L->ci != L->base_ci))
        return resume_error(L, "cannot resume non-suspended coroutine", 1);

    L->nCcalls = from ? from->nCcalls : 0;
    if (L->nCcalls >= LUAI_MAXCCALLS)
        return resume_error(L, "C stack overflow", 1);

    L->baseCcalls = ++L->nCcalls;
    L->isactive = true;

    luaC_threadbarrier(L);

    status = LUA_ERRRUN;

    CallInfo* ch = NULL;
    while (status != 0 && (ch = resume_findhandler(L)) != NULL)
    {
        L->status = cast_byte(status);
        status = luaD_rawrunprotected(L, resume_handle, ch);
    }

    resume_finish(L, status);
    --L->nCcalls;
    return L->status;
}

int lua_yield(lua_State* L, int nresults)
{
    if (L->nCcalls > L->baseCcalls)
        luaG_runerror(L, "attempt to yield across metamethod/C-call boundary");
    L->base = L->top - nresults; // protect stack slots below
    L->status = LUA_YIELD;
    return -1;
}

int lua_break(lua_State* L)
{
    if (L->nCcalls > L->baseCcalls)
        luaG_runerror(L, "attempt to break across metamethod/C-call boundary");
    L->status = LUA_BREAK;
    return -1;
}

int lua_isyieldable(lua_State* L)
{
    return (L->nCcalls <= L->baseCcalls);
}

static void callerrfunc(lua_State* L, void* ud)
{
    StkId errfunc = cast_to(StkId, ud);

    setobj2s(L, L->top, L->top - 1);
    setobj2s(L, L->top - 1, errfunc);
    incr_top(L);

    if (DFFlag::LuauErrorYield)
        luaD_callny(L, L->top - 2, 1);
    else
        luaD_call(L, L->top - 2, 1);
}

static void restore_stack_limit(lua_State* L)
{
    LUAU_ASSERT(L->stack_last - L->stack == L->stacksize - EXTRA_STACK);
    if (L->size_ci > LUAI_MAXCALLS)
    { // there was an overflow?
        int inuse = cast_int(L->ci - L->base_ci);
        if (inuse + 1 < LUAI_MAXCALLS) // can `undo' overflow?
            luaD_reallocCI(L, LUAI_MAXCALLS);
    }
}

int luaD_pcall(lua_State* L, Pfunc func, void* u, ptrdiff_t old_top, ptrdiff_t ef)
{
    unsigned short oldnCcalls = L->nCcalls;
    unsigned short oldbaseCcalls = L->baseCcalls;
    ptrdiff_t old_ci = saveci(L, L->ci);
    bool oldactive = L->isactive;
    int status = luaD_rawrunprotected(L, func, u);
    if (status != 0)
    {
        int errstatus = status;

        // call user-defined error function (used in xpcall)
        if (ef)
        {
            // push error object to stack top if it's not already there
            if (status != LUA_ERRRUN)
                seterrorobj(L, status, L->top);

            // if errfunc fails, we fail with "error in error handling" or "not enough memory"
            int err = luaD_rawrunprotected(L, callerrfunc, restorestack(L, ef));

            // in general we preserve the status, except for cases when the error handler fails
            // out of memory is treated specially because it's common for it to be cascading, in which case we preserve the code
            if (err == 0)
                errstatus = LUA_ERRRUN;
            else if (status == LUA_ERRMEM && err == LUA_ERRMEM)
                errstatus = LUA_ERRMEM;
            else
                errstatus = status = LUA_ERRERR;
        }

        // since the call failed with an error, we might have to reset the 'active' thread state
        if (!oldactive)
            L->isactive = false;

        bool yieldable = L->nCcalls <= L->baseCcalls; // Inlined logic from 'lua_isyieldable' to avoid potential for an out of line call.

        // restore nCcalls and baseCcalls before calling the debugprotectederror callback which may rely on the proper value to have been restored.
        L->nCcalls = oldnCcalls;
        L->baseCcalls = oldbaseCcalls;

        // an error occurred, check if we have a protected error callback
        if (yieldable && L->global->cb.debugprotectederror)
        {
            L->global->cb.debugprotectederror(L);

            // debug hook is only allowed to break
            if (L->status == LUA_BREAK)
                return 0;
        }

        StkId oldtop = restorestack(L, old_top);
        luaF_close(L, oldtop); // close eventual pending closures
        seterrorobj(L, errstatus, oldtop);
        L->ci = restoreci(L, old_ci);
        L->base = L->ci->base;
        restore_stack_limit(L);
    }
    return status;
}
