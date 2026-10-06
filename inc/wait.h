// Net Helper 9300: blocking-style waits for the worker threads (a nested active scheduler runs
// while waiting, so active objects of the TLS / Bluetooth code keep running), with timeouts and a
// stop request from the UI thread.

#ifndef WAIT_H
#define WAIT_H

#include <e32base.h>

// ------------------------------------------------------------------ blocking waits

class MCancelIo
    {
public:
    virtual void CancelIo() = 0;
    };

class CTimeout : public CTimer
    {
public:
    CTimeout() : CTimer(EPriorityHigh) {}
    void ConstructL() { CTimer::ConstructL(); CActiveScheduler::Add(this); }
    void RunL() { iFired = ETrue; if (iOp) iOp->CancelIo(); }
    MCancelIo* iOp;
    TBool iFired;
    };

// Issue a request with Status(), then Wait(): runs the scheduler until it completes (or times out).
class CWaiter : public CActive
    {
public:
    CWaiter() : CActive(EPriorityStandard) { CActiveScheduler::Add(this); }
    void ConstructL() { iTimeout = new (ELeave) CTimeout; iTimeout->ConstructL(); iWait = new (ELeave) CActiveSchedulerWait; }
    ~CWaiter() { delete iTimeout; delete iWait; }
    TRequestStatus& Status() { return iStatus; }
    TInt Wait(TInt aMs, MCancelIo* aOp)
        {
        SetActive();
        iCurrent = aOp;
        if (iStopping && aOp) aOp->CancelIo();          // shutting down: don't start waiting
        iTimeout->iFired = EFalse;
        iTimeout->iOp = aOp;
        if (aMs > 0 && aOp) iTimeout->After(aMs * 1000);
        iWait->Start();
        iCurrent = NULL;
        iTimeout->Cancel();
        TInt r = iStatus.Int();
        if (iTimeout->iFired && r != KErrNone) r = KErrTimedOut;
        return r;
        }
private:
public:
    void Stop()
        {
        iStopping = ETrue;
        if (iCurrent) iCurrent->CancelIo();
        }
    TBool Stopping() const { return iStopping; }
private:
    void RunL() { iWait->AsyncStop(); }
    void DoCancel() {}
    MCancelIo* iCurrent;
    TBool iStopping;
    CTimeout* iTimeout;
    CActiveSchedulerWait* iWait;
    };

// Waits for the UI thread's stop request (RThread::RequestComplete on Exit).
class CStopper : public CActive
    {
public:
    CStopper(CWaiter& aW) : CActive(EPriorityHigh), iW(aW) { CActiveScheduler::Add(this); iStatus = KRequestPending; SetActive(); }
    ~CStopper() { Cancel(); }
    TRequestStatus* StatusPtr() { return &iStatus; }
private:
    void RunL() { iW.Stop(); }
    void DoCancel() { TRequestStatus* s = &iStatus; User::RequestComplete(s, KErrCancel); }
    CWaiter& iW;
    };

#endif
