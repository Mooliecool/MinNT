/***
*ghandler.c
*
*	Copyright (c) 1990-1995, Microsoft Corporation. All rights reserved.
*
*Purpose:
*
*   This module implements the C specific exception handler that provides
*   structured exception handling for code generated by the GEM compiler.
*
*Revision History:
*
****/

#include "nt.h"
extern _NLG_Notify(PVOID, PVOID, ULONG);


//
// Define procedure prototypes for exception filter and termination handler
// execution routines defined in jmpunwnd.s.
//

LONG
__C_ExecuteExceptionFilter (
    PEXCEPTION_POINTERS ExceptionPointers,
    EXCEPTION_FILTER ExceptionFilter,
    ULONG EstablisherFrame
    );

ULONG
__C_ExecuteTerminationHandler (
    BOOLEAN AbnormalTermination,
    TERMINATION_HANDLER TerminationHandler,
    ULONG EstablisherFrame
    );

//
// Define local procedure prototypes.
//

EXCEPTION_DISPOSITION
_OtsCSpecificHandler (
    IN PEXCEPTION_RECORD ExceptionRecord,
    IN PVOID EstablisherFrame,
    IN OUT PCONTEXT ContextRecord,
    IN OUT PDISPATCHER_CONTEXT DispatcherContext
    );

ULONG
_OtsLocalFinallyUnwind (
    IN PSEH_CONTEXT SehContext,
    IN PSEH_BLOCK TargetSeb,
    IN PVOID RealFramePointer
    );

//
// Define local macros.
//

#define IS_EXCEPT(Seb) ((Seb)->JumpTarget != 0)
#define IS_FINALLY(Seb) ((Seb)->JumpTarget == 0)

//
// Initialize an exception record for the unwind with the SEB of the target
// included as one information parameter. This is done so that the target
// frame of the unwind may execute all the finally handlers necessary given
// the SEB pointer at the unwind target.
//

#define MODIFY_UNWIND_EXCEPTION_RECORD(ExceptionRecord, Seb) { \
    ExceptionRecord->ExceptionCode = STATUS_UNWIND; \
    ExceptionRecord->ExceptionFlags = EXCEPTION_UNWINDING; \
    ExceptionRecord->ExceptionRecord = NULL; \
    ExceptionRecord->ExceptionAddress = 0; \
    ExceptionRecord->NumberParameters = 1; \
    ExceptionRecord->ExceptionInformation[0] = (ULONG)(Seb); \
    }

EXCEPTION_DISPOSITION
_OtsCSpecificHandler (
    IN PEXCEPTION_RECORD ExceptionRecord,
    IN PVOID EstablisherFrame,
    IN OUT PCONTEXT ContextRecord,
    IN OUT PDISPATCHER_CONTEXT DispatcherContext
    )

/*++

Routine Description:

    This function walks up the list of SEB's associated with the specified
    procedure and calls except filters and finally handlers as necessary.

    It is called in two different contexts:
        (i)  by the exception dispatcher after an exception is raised
        (ii) by the unwinder during an unwind operation

    In the first case, is searches the SEB list for except filters to evaluate.
    In the second case, it searches for finally handlers to execute.

Arguments:

    ExceptionRecord - Supplies a pointer to an exception record.

    EstablisherFrame - Supplies a (virtual frame) pointer to the frame of the
        establisher function.

    ContextRecord - Supplies a pointer to a context record.

    DispatcherContext - Supplies a pointer to the exception dispatcher or
        unwind dispatcher context.

Return Value:

    If the exception is handled by one of the exception filter routines, then
    there is no return from this routine and RtlUnwind is called. Otherwise,
    an exception disposition value of continue execution or continue search is
    returned.

Notes:
    In context (i) there are 3 possibilities:

        (a) If an exception filter returns a value greater that 0 (meaning
            that the associated handler should be invoked) there is no
            return from this function. RtlUnwind is called to unwind the
            stack to the exception handler corresponding to that filter.

        (b) If an exception filter returns a value less than 0 (meaning
            that the exception should be dismissed), this routine returns
            value ExceptionContinueExecution.

        (c) If every filter returns value 0 (meaning that the search for a
            handler should continue elsewhere), this function returns
            ExceptionContinueSearch.

    In context (ii) there are 2 possibilities:

        (d) If no branches are detected out of finally handlers, this
            function returns ExceptionContinueSearch.

        (e) If a branch is detected out of a finally handler, there is no
            return from this routine. RtlUnwind is called to unwind to the
            branch target (and cancel the current unwind).

    There may be long jumps out of both except filters and finally handlers
    in which case this routine will be peeled off the stack without returning.

--*/

{

    ULONG ContinuationAddress;
    EXCEPTION_FILTER ExceptionFilter;
    PVOID ExceptionHandler;
    EXCEPTION_POINTERS ExceptionPointers;
    LONG FilterValue;
    ULONG RealFramePointer;
    PSEH_BLOCK Seb;
    PSEH_CONTEXT SehContext;
    PSEH_BLOCK TargetSeb;
    TERMINATION_HANDLER TerminationHandler;

    //
    // Get the address of the SEH context which is at some negative offset
    // from the virtual frame pointer. For GEM, the handler data field of
    // the function entry contains that offset. The current SEB pointer and
    // the RFP (static link) are obtained from the SEH context.
    //

    SehContext = (PSEH_CONTEXT)((ULONG)EstablisherFrame +
        (LONG)DispatcherContext->FunctionEntry->HandlerData);
    RealFramePointer = SehContext->RealFramePointer;

    //
    // If this is a dispatching context, walk up the list of SEBs evaluating
    // except filters.
    //

    if (IS_DISPATCHING(ExceptionRecord->ExceptionFlags)) {

        //
        // Set up the ExceptionPointers structure that is used by except
        // filters to obtain data for the GetExceptionInformation intrinsic
        // function. Copy the current SEB pointer into a local variable
        // because the real SEB pointer is only modified in unwind contexts.
        //

        ExceptionPointers.ExceptionRecord = ExceptionRecord;
        ExceptionPointers.ContextRecord = ContextRecord;

        for (Seb = SehContext->CurrentSeb; Seb != NULL; Seb = Seb->ParentSeb) {
            if (IS_EXCEPT(Seb)) {

                //
                // This is an except filter. Get the addresses of the filter
                // and exception handler from the SEB, then call the except
                // filter.
                //

                ExceptionFilter = (EXCEPTION_FILTER)Seb->HandlerAddress;
                ExceptionHandler = (PVOID)Seb->JumpTarget;

                FilterValue = __C_ExecuteExceptionFilter(&ExceptionPointers,
                                                         ExceptionFilter,
                                                         RealFramePointer);

                //
                // If the except filter < 0, dismiss the exception. If > 0,
                // store the exception code on the stack for the except
                // handler, modify the given ExceptionRecord so that finally
                // handlers will be called properly during the unwind, then
                // unwind down to the except handler. If = 0, resume the
                // search for except filters.
                //

                if (FilterValue < 0) {
                    return ExceptionContinueExecution;

                } else if (FilterValue > 0) {
                    SehContext->ExceptionCode = ExceptionRecord->ExceptionCode;
                    MODIFY_UNWIND_EXCEPTION_RECORD(ExceptionRecord,
                                                   Seb->ParentSeb);
                    //
                    // Notify the (possibly running) debugger of the unwind so it
					// can step over the exception and step into the handler.
                    //

                    _NLG_Notify((PVOID)ExceptionHandler, EstablisherFrame, 0x1);

                    RtlUnwind(EstablisherFrame,
                              ExceptionHandler,
                              ExceptionRecord,
                              0);
                }
            }
        }

    } else if (!IS_TARGET_UNWIND(ExceptionRecord->ExceptionFlags)) {

        //
        // This is an unwind but is not the target frame. Since the function
        // is being terminated, finally handlers for all try bodies that are
        // presently in scope must be executed. Walk up the SEB list all the
        // way to the top executing finally handlers. This corresponds to
        // exiting all try bodies that are presently in scope.
        //

        while (SehContext->CurrentSeb != NULL) {

            //
            // Get the address of the SEB and then update the SEH context.
            //

            Seb = SehContext->CurrentSeb;
            SehContext->CurrentSeb = Seb->ParentSeb;

            if (IS_FINALLY(Seb)) {

                //
                // This is a finally handler. Get the address of the handler
                // from the SEB and call the finally handler.
                //

                TerminationHandler = (TERMINATION_HANDLER)Seb->HandlerAddress;
                ContinuationAddress =
                    __C_ExecuteTerminationHandler(TRUE,
                                                  TerminationHandler,
                                                  RealFramePointer);

                //
                // If the finally handler returns a non-zero result, there
                // was a branch out of the handler (to that address) and this
                // routine should unwind to that target.
                //

                if (ContinuationAddress != 0) {
                    MODIFY_UNWIND_EXCEPTION_RECORD(ExceptionRecord,
                                                   SehContext->CurrentSeb);
                    //
                    // Notify the (possibly running) debugger of the unwind so it
					// can step over the branch out and step into the branch target.
                    //

                    _NLG_Notify((PVOID)ContinuationAddress, EstablisherFrame, 0x0);

                    RtlUnwind(EstablisherFrame,
                              (PVOID)ContinuationAddress,
                              ExceptionRecord,
                              0);
                }
            }
        }

    } else {

        //
        // This is the target frame of an unwind. Since the target may be
        // in a different try scope than the one defined by the current SEB
        // pointer, finally handlers between the two scopes must execute.
        // Walk up the SEB list from the current SEB to the target SEB and
        // execute all finally handlers encountered.
        //

        TargetSeb = (PSEH_BLOCK)ExceptionRecord->ExceptionInformation[0];
        ContinuationAddress = _OtsLocalFinallyUnwind(SehContext,
                                                     TargetSeb,
                                                     (PVOID)RealFramePointer);
        if (ContinuationAddress != 0) {

            //
            // A non-zero result indicates there was a branch out of a
            // finally handler that was being executed during the unwind.
            // This routine should unwind to that address.
            //

            MODIFY_UNWIND_EXCEPTION_RECORD(ExceptionRecord,
                                           SehContext->CurrentSeb);
            //
            // Notify the (possibly running) debugger of the unwind so it
			// can step over the branch out and step into the target.
            //

            _NLG_Notify((PVOID)ContinuationAddress, EstablisherFrame, 0x0);

           RtlUnwind(EstablisherFrame,
                      (PVOID)ContinuationAddress,
                      ExceptionRecord,
                      0);
        }
    }

    //
    // Continue search for exception or termination handlers.
    //

    return ExceptionContinueSearch;
}

ULONG
_OtsLocalFinallyUnwind (
    IN PSEH_CONTEXT SehContext,
    IN PSEH_BLOCK TargetSeb,
    IN PVOID RealFramePointer
    )

/*++

Routine Description:

    This function walks up the SEB tree of the current procedure from the
    current SEB to the target SEB and executes all the finally handlers it
    encounters.

    Calls to this function are inserted into user code by the compiler when
    there are branches out of guarded regions that may require finally
    handlers to execute.

    This function is also called from _OtsCSpecificHandler when the target
    frame is reached during an unwind operation. There may be finally handlers
    that should execute before resuming execution at the unwind target.

Arguments:

    SehContext - Supplies the address of the SEH context structure which is
        located in the stack frame.

    TargetSeb - Supplies the address of the SEB corresponding to the branch
        target address.

    RealFramePointer - Supplies the (real frame) pointer of the establisher
        frame, which is the current stack frame. This is used to set up the
        static link if a finally handler is invoked.

Return Value:

    If a branch out of a finally handler is detected, the function value is
    the address of the branch target. Otherwise, the function value is zero.

--*/

{

    ULONG ContinuationAddress;
    BOOLEAN Nested;
    PSEH_BLOCK Seb;
    TERMINATION_HANDLER TerminationHandler;

    //
    // If the SEB pointers are the same, no finally handlers need to execute.
    // The branch is to a target location in the same guarded scope.
    //

    if (SehContext->CurrentSeb == TargetSeb) {
        return 0;
    }

    //
    // If the current SEB scope is not nested within the target SEB scope, no
    // finally handlers need to execute. Reset the current SEB pointer to the
    // target SEB pointer and return.
    //

    Nested = FALSE;
    Seb = SehContext->CurrentSeb;

    while (Seb != NULL) {
        Seb = Seb->ParentSeb;
        if (Seb == TargetSeb) {
            Nested = TRUE;
            break;
        }
    }
    if (Nested == FALSE) {
        SehContext->CurrentSeb = TargetSeb;
        return 0;
    }

    //
    // Walk up the list of SEB blocks executing finally handlers. If a branch
    // out of a finally is encountered along the way, return the target
    // address, otherwise return 0.
    //

    while (SehContext->CurrentSeb != TargetSeb) {

        //
        // Get the address of the SEB and then update the SEH context.
        //

        Seb = SehContext->CurrentSeb;
        SehContext->CurrentSeb = Seb->ParentSeb;

        if (IS_FINALLY(Seb)) {
            TerminationHandler = (TERMINATION_HANDLER)Seb->HandlerAddress;
            ContinuationAddress =
                __C_ExecuteTerminationHandler(TRUE,
                                              TerminationHandler,
                                              (ULONG)RealFramePointer);
            if (ContinuationAddress != 0) {
                return ContinuationAddress;
            }
        }
    }
    return 0;
}
