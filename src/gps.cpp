// Net Helper 9300, the GPS thread: reads the Bluetooth GPS (an Android phone sharing its GPS as
// NMEA over the Serial Port Profile, or a GPS receiver) natively, outside the phone's Java.
//
// Java reading Bluetooth while downloading made every download wait seconds and crashed
// jes-java-comms (E32USER-CBase 40, Probe 3.5). Here the GPS has its own thread: it finds the
// serial port service (SDP, UUID 0x1101) on the address the Java app asks for, connects RFCOMM to
// its channel, keeps the latest GGA and RMC sentences and reconnects when the link drops. The HTTP
// worker hands them out at /gps; without a /gps request for a minute the GPS is let go.

#include <e32base.h>
#include <es_sock.h>
#include <bt_sock.h>
#include <btsdp.h>
#include "nethelper.h"
#include "wait.h"

const TInt KSdpMs = 20000, KConnectMs = 20000, KNoDataMs = 10000;
const TInt KIdleLetGoSecs = 60;

// ------------------------------------------------------------------ SDP: the SPP service's channel

class CSdpQuery : public CBase, public MSdpAgentNotifier, public MSdpAttributeValueVisitor, public MCancelIo
    {
public:
    ~CSdpQuery() { delete iAgent; delete iPattern; }
    // Starts the search; aStatus completes with KErrNone (iChannel set) or an error.
    void StartL(const TBTDevAddr& aAddr, TRequestStatus& aStatus)
        {
        delete iAgent; iAgent = NULL;
        delete iPattern; iPattern = NULL;
        iChannel = -1;
        iNextIsChannel = EFalse;
        iStatus = &aStatus;
        aStatus = KRequestPending;
        iAgent = CSdpAgent::NewL(*this, aAddr);
        iPattern = CSdpSearchPattern::NewL();
        iPattern->AddL(0x1101);              // Serial Port
        iAgent->SetRecordFilterL(*iPattern);
        iAgent->NextRecordRequestL();
        }
    void CancelIo()
        {
        if (iAgent) iAgent->Cancel();
        Complete(KErrCancel);
        }
    TInt iChannel;
private:
    void Complete(TInt aErr)
        {
        if (!iStatus) return;
        TRequestStatus* s = iStatus;
        iStatus = NULL;
        User::RequestComplete(s, aErr);
        }
    // MSdpAgentNotifier
    void NextRecordRequestComplete(TInt aError, TSdpServRecordHandle aHandle, TInt aTotal)
        {
        if (aError != KErrNone || aTotal == 0) { Complete(aError == KErrNone || aError == KErrEof ? KErrNotFound : aError); return; }
        TRAPD(err, iAgent->AttributeRequestL(aHandle, KSdpAttrIdProtocolDescriptorList));
        if (err != KErrNone) Complete(err);
        }
    void AttributeRequestResult(TSdpServRecordHandle, TSdpAttributeID, CSdpAttrValue* aValue)
        {
        TRAPD(err, aValue->AcceptVisitorL(*this));
        delete aValue;
        }
    void AttributeRequestComplete(TSdpServRecordHandle, TInt aError)
        {
        if (iChannel > 0) { Complete(KErrNone); return; }
        if (aError != KErrNone) { Complete(aError); return; }
        TRAPD(err, iAgent->NextRecordRequestL());        // this record had no RFCOMM channel: the next one
        if (err != KErrNone) Complete(err);
        }
    // MSdpAttributeValueVisitor: ProtocolDescriptorList = ((L2CAP), (RFCOMM, channel))
    void VisitAttributeValueL(CSdpAttrValue& aValue, TSdpElementType aType)
        {
        if (aType == ETypeUUID) iNextIsChannel = aValue.UUID() == TUUID(KRFCOMM);
        else if (aType == ETypeUint && iNextIsChannel) { iChannel = aValue.Uint(); iNextIsChannel = EFalse; }
        }
    void StartListL(CSdpAttrValueList&) {}
    void EndListL() {}

    CSdpAgent* iAgent;
    CSdpSearchPattern* iPattern;
    TRequestStatus* iStatus;
    TBool iNextIsChannel;
    };

// ------------------------------------------------------------------ the GPS reader

class CSleepIo : public CBase, public MCancelIo
    {
public:
    void CancelIo() { iTimer.Cancel(); }
    RTimer iTimer;
    };

class CBtIo : public CBase, public MCancelIo
    {
public:
    void CancelIo() { iSock.CancelAll(); }
    RSocket iSock;
    TBool iOpen;
    };

class CGps : public CBase
    {
public:
    CGps(TNetStats& aStats) : iStats(aStats), iGps(aStats.iGps) {}
    ~CGps();
    void ConstructL();
    void RunL();
private:
    void SetState(const TDesC8& aState, const TDesC8& aInfo);
    TBool Wanted(TBTDevAddr& aAddr);
    TInt Session(const TBTDevAddr& aAddr);
    void Line(const TDesC8& aLine);
    void Sleep(TInt aMs);
    TNetStats& iStats;
    TGpsState& iGps;
    RSocketServ iSs;
    CWaiter* iW;
    CStopper* iStopper;
    CSdpQuery* iSdp;
    CBtIo* iBt;
    CSleepIo* iSleep;
    TBuf8<512> iRecv;
    TSockXfrLength iXfr;
    TBuf8<200> iLine;
    TInt iStep;                 // 1 SDP search, 2 RFCOMM connect, 3 connected
    };

CGps::~CGps()
    {
    iGps.iStop = NULL;
    delete iStopper;
    if (iBt) { if (iBt->iOpen) iBt->iSock.Close(); delete iBt; }
    delete iSdp;
    if (iSleep) { iSleep->iTimer.Close(); delete iSleep; }
    delete iW;
    iSs.Close();
    }

void CGps::ConstructL()
    {
    iW = new (ELeave) CWaiter;
    iW->ConstructL();
    iStopper = new (ELeave) CStopper(*iW);
    iGps.iStop = iStopper->StatusPtr();
    iSdp = new (ELeave) CSdpQuery;
    iBt = new (ELeave) CBtIo;
    iSleep = new (ELeave) CSleepIo;
    User::LeaveIfError(iSleep->iTimer.CreateLocal());
    User::LeaveIfError(iSs.Connect());
    SetState(_L8("idle"), _L8("waiting for a Java app to ask for the GPS"));
    }

void CGps::SetState(const TDesC8& aState, const TDesC8& aInfo)
    {
    iGps.iLock.Wait();
    iGps.iState = aState.Left(iGps.iState.MaxLength());
    iGps.iInfo = aInfo.Left(iGps.iInfo.MaxLength());
    iGps.iLock.Signal();
    TBuf<140> l;
    l.Copy(_L8("GPS: "));
    TBuf<120> i;
    i.Copy(aInfo.Left(110));
    l.Append(i);
    AddStatLine(iStats, l);
    }

// Whether a Java app wants the GPS (asked within the last minute); its address in aAddr.
TBool CGps::Wanted(TBTDevAddr& aAddr)
    {
    TBuf8<12> a;
    TBool wanted;
    TTime last;
    iGps.iLock.Wait();
    a = iGps.iAddr;
    wanted = iGps.iWanted;
    last = iGps.iLastAsk;
    iGps.iLock.Signal();
    if (!wanted || a.Length() != 12) return EFalse;
    TTime now;
    now.HomeTime();
    TTimeIntervalSeconds s;
    if (now.SecondsFrom(last, s) == KErrNone && s.Int() > KIdleLetGoSecs) return EFalse;
    for (TInt i = 0; i < 6; i++)
        {
        TLex8 lex(a.Mid(i * 2, 2));
        TUint v;
        if (lex.Val(v, EHex) != KErrNone) return EFalse;
        aAddr[i] = (TUint8) v;
        }
    return ETrue;
    }

// A pause the stop request can end (it runs the scheduler like the other waits).
void CGps::Sleep(TInt aMs)
    {
    iSleep->iTimer.After(iW->Status(), aMs * 1000);
    iW->Wait(0, iSleep);
    }

void CGps::RunL()
    {
    TInt failures = 0;
    while (!iW->Stopping())
        {
        TBTDevAddr addr;
        if (!Wanted(addr))
            {
            if (iGps.iState.Compare(_L8("idle")) != 0) SetState(_L8("idle"), _L8("not asked for a minute: Bluetooth let go"));
            Sleep(500);
            continue;
            }
        iStep = 0;
        TInt err = Session(addr);
        if (iW->Stopping()) break;
        // the link ended or failed: again, a bit later each time (at most 10 s; 15 s when blocked)
        failures = err == KErrNone ? 0 : failures + 1;
        TInt wait = Min(10, 2 + failures * 2);
        TBuf8<200> info;
        const TDesC8& step = iStep == 1 ? _L8("finding the GPS service") : iStep == 2 ? _L8("connecting") : _L8("connection");
        if (err == KErrHardwareNotAvailable) SetState(_L8("btoff"), _L8("Bluetooth is off"));
        else if (err <= -6000 && err > -6100 && err != -6004 && iStep <= 2)
            {
            // Bluetooth chip errors (seen: -6031 "unspecified") while reaching the Android phone: on
            // the 9300 the usual cause is a Bluetooth link to a PC (PC Suite), which blocks others
            wait = 15;
            info.Format(_L8("can't reach the GPS phone (Bluetooth error %d). Is the 9300 connected to a PC over Bluetooth? End that connection; again in %d s"), err, wait);
            SetState(_L8("blocked"), info);
            }
        else if (err == -6004) { info.Format(_L8("the GPS phone doesn't answer (out of range or its Bluetooth off), again in %d s"), wait); SetState(_L8("error"), info); }
        else if (err == KErrNotFound && iStep == 1) { info.Format(_L8("the phone offers no GPS sharing: switch it on (GPS NMEA Tether); again in %d s"), wait); SetState(_L8("error"), info); }
        else { info.Format(_L8("%S failed (%d), again in %d s"), &step, err, wait); SetState(_L8("error"), info); }
        for (TInt i = 0; i < wait * 2 && !iW->Stopping(); i++) Sleep(500);
        }
    }

// One connection: SDP search, connect, read until the link drops or nobody wants the GPS.
TInt CGps::Session(const TBTDevAddr& aAddr)
    {
    iStep = 1;
    SetState(_L8("searching"), _L8("looking for the GPS service"));
    TRAPD(err, iSdp->StartL(aAddr, iW->Status()));
    if (err != KErrNone) return err;
    err = iW->Wait(KSdpMs, iSdp);
    if (err != KErrNone) return err;
    TBuf8<60> info;
    info.Format(_L8("connecting to channel %d"), iSdp->iChannel);
    iStep = 2;
    SetState(_L8("connecting"), info);

    err = iBt->iSock.Open(iSs, KBTAddrFamily, KSockStream, KRFCOMM);
    if (err != KErrNone) return err;
    iBt->iOpen = ETrue;
    TBTSockAddr sa;
    sa.SetBTAddr(aAddr);
    sa.SetPort(iSdp->iChannel);
    iBt->iSock.Connect(sa, iW->Status());
    err = iW->Wait(KConnectMs, iBt);
    if (err == KErrNone)
        {
        iGps.iLock.Wait();
        iGps.iConnects++;
        iGps.iChannel = iSdp->iChannel;
        iGps.iLock.Signal();
        iStep = 3;
        info.Format(_L8("connected (channel %d), waiting for data"), iSdp->iChannel);
        SetState(_L8("connected"), info);
        iLine.Zero();
        TBTDevAddr cur;
        while (!iW->Stopping() && Wanted(cur) && cur == aAddr)     // until let go or another address
            {
            iBt->iSock.RecvOneOrMore(iRecv, 0, iW->Status(), iXfr);
            err = iW->Wait(KNoDataMs, iBt);
            for (TInt i = 0; i < iRecv.Length(); i++)
                {
                TUint8 c = iRecv[i];
                if (c == '\n' || c == '\r') { if (iLine.Length() > 0) Line(iLine); iLine.Zero(); }
                else if (iLine.Length() < iLine.MaxLength()) iLine.Append(c);
                }
            if (err != KErrNone) break;
            }
        }
    iBt->iSock.Close();
    iBt->iOpen = EFalse;
    return err;
    }

void CGps::Line(const TDesC8& aLine)
    {
    if (aLine.Length() < 7 || aLine[0] != '$') return;
    TPtrC8 kind = aLine.Mid(3, 3);
    TTime now;
    now.HomeTime();
    iGps.iLock.Wait();
    iGps.iSentences++;
    iGps.iLastData = now;
    if (kind.Compare(_L8("GGA")) == 0) iGps.iGga = aLine.Left(iGps.iGga.MaxLength());
    else if (kind.Compare(_L8("RMC")) == 0) iGps.iRmc = aLine.Left(iGps.iRmc.MaxLength());
    TBool first = iGps.iSentences == 1;
    iGps.iLock.Signal();
    if (first) SetState(_L8("connected"), _L8("receiving NMEA"));
    if (iGps.iSentences % 20 == 0) iStats.iChanged++;
    }

// ------------------------------------------------------------------ thread

LOCAL_C void RunGpsL(TNetStats& aStats)
    {
    CGps* g = new (ELeave) CGps(aStats);
    CleanupStack::PushL(g);
    g->ConstructL();
    g->RunL();
    CleanupStack::PopAndDestroy(g);
    }

TInt NetGpsThread(TAny* aStats)
    {
    TNetStats& stats = *(TNetStats*) aStats;
    CTrapCleanup* cleanup = CTrapCleanup::New();
    CActiveScheduler* as = new CActiveScheduler;
    if (!cleanup || !as) return KErrNoMemory;
    CActiveScheduler::Install(as);
    TRAPD(err, RunGpsL(stats));
    if (err != KErrNone)
        {
        TBuf<60> l;
        l.Format(_L("GPS thread stopped: error %d"), err);
        AddStatLine(stats, l);
        }
    delete as;
    delete cleanup;
    return err;
    }
