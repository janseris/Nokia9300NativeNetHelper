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
#include <in_sock.h>
#include "nethelper.h"
#include "wait.h"

const TInt KSdpMs = 20000, KConnectMs = 20000, KNoDataMs = 15000;
const TInt KMaxChannels = 8;
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
        iCount = 0;
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
    // the RFCOMM channels of all the serial port services offered (an Android phone can offer several,
    // and not every one sends the GPS: seen channel 6 connect and stay silent, channel 11 send NMEA)
    TInt iChannels[KMaxChannels];
    TInt iCount;
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
        if (aError != KErrNone || aTotal == 0)
            {
            if (iCount > 0 && (aError == KErrEof || aError == KErrNone)) Complete(KErrNone);   // no more records
            else Complete(aError == KErrNone || aError == KErrEof ? KErrNotFound : aError);
            return;
            }
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
        if (aError != KErrNone) { Complete(iCount > 0 ? KErrNone : aError); return; }
        if (iCount >= KMaxChannels) { Complete(KErrNone); return; }
        TRAPD(err, iAgent->NextRecordRequestL());        // all the records: every serial port's channel
        if (err != KErrNone) Complete(iCount > 0 ? KErrNone : err);
        }
    // MSdpAttributeValueVisitor: ProtocolDescriptorList = ((L2CAP), (RFCOMM, channel))
    void VisitAttributeValueL(CSdpAttrValue& aValue, TSdpElementType aType)
        {
        if (aType == ETypeUUID) iNextIsChannel = aValue.UUID() == TUUID(KRFCOMM);
        else if (aType == ETypeUint && iNextIsChannel)
            {
            iNextIsChannel = EFalse;
            TInt ch = aValue.Uint();
            for (TInt i = 0; i < iCount; i++) if (iChannels[i] == ch) return;
            if (iCount < KMaxChannels) iChannels[iCount++] = ch;
            }
        }
    void StartListL(CSdpAttrValueList&) {}
    void EndListL() {}

    CSdpAgent* iAgent;
    CSdpSearchPattern* iPattern;
    TRequestStatus* iStatus;
    TBool iNextIsChannel;
    };

// ------------------------------------------------------------------ /gps answers

// The /gps answer (also used by the HTTP worker on 8123): query "?addr=<12 hex>" keeps the GPS
// wanted, "?stop=1" lets it go. Lines state= info= age= sentences= channel= connects=, then the
// latest GGA and RMC sentences.
void GpsReply(TGpsState& g, const TDesC8& aQuery, TDes8& aBody)
    {
    TTime now;
    now.HomeTime();
    TInt a = aQuery.Find(_L8("addr="));
    g.iLock.Wait();
    if (aQuery.Find(_L8("stop=1")) >= 0) g.iWanted = EFalse;
    else if (a >= 0 && aQuery.Length() >= a + 5 + 12)
        {
        g.iAddr = aQuery.Mid(a + 5, 12);
        g.iAddr.UpperCase();
        g.iWanted = ETrue;
        g.iLastAsk = now;
        }
    TInt age = -1;
    if (g.iLastData.Int64() != TInt64(0))
        {
        TInt64 us = now.MicroSecondsFrom(g.iLastData).Int64();
        us /= 1000;
        age = us.GetTInt();
        }
    aBody.AppendFormat(_L8("state=%S\ninfo=%S\nage=%d\nsentences=%d\nchannel=%d\nconnects=%d\n"),
        &g.iState, &g.iInfo, age, g.iSentences, g.iChannel, g.iConnects);
    if (g.iGga.Length() > 0) { aBody.Append(g.iGga); aBody.Append(_L8("\n")); }
    if (g.iRmc.Length() > 0) { aBody.Append(g.iRmc); aBody.Append(_L8("\n")); }
    g.iLock.Signal();
    }

// A tiny server for /gps on 127.0.0.1:8124 in the GPS thread itself (an active object, it runs
// while the thread waits for Bluetooth), so a GPS request never waits behind a tile download in
// the HTTP worker (Probe 3.6: up to 54 s on 8123).
class CGpsServer : public CActive
    {
public:
    CGpsServer(TGpsState& aGps, RSocketServ& aSs) : CActive(EPriorityHigh), iGps(aGps), iSs(aSs) { CActiveScheduler::Add(this); }
    ~CGpsServer() { Cancel(); if (iConnOpen) iConn.Close(); iListen.Close(); }
    void ConstructL()
        {
        User::LeaveIfError(iListen.Open(iSs, KAfInet, KSockStream, KProtocolInetTcp));
        iListen.SetOpt(KSoReuseAddr, KSolInetIp, 1);
        TInetAddr addr(INET_ADDR(127, 0, 0, 1), KNetHelperPort + 1);
        User::LeaveIfError(iListen.Bind(addr));
        User::LeaveIfError(iListen.Listen(4));
        Next();
        }
private:
    enum { EAccept, ERead, EWrite };
    void Next()
        {
        if (iConnOpen) { iConn.Close(); iConnOpen = EFalse; }
        if (iConn.Open(iSs) != KErrNone) return;
        iConnOpen = ETrue;
        iReq.Zero();
        iState = EAccept;
        iListen.Accept(iConn, iStatus);
        SetActive();
        }
    void RunL()
        {
        if (iStatus.Int() != KErrNone) { if (iState == EAccept && iStatus.Int() != KErrCancel) { User::After(100000); } Next(); return; }
        if (iState == EWrite) { Next(); return; }
        if (iState == ERead && iReq.Length() + iRecv.Length() <= iReq.MaxLength()) iReq.Append(iRecv);
        if (iState == EAccept || (iReq.Find(_L8("\r\n\r\n")) < 0 && iReq.Length() < iReq.MaxLength()))
            {
            iState = ERead;
            iConn.RecvOneOrMore(iRecv, 0, iStatus, iXfr);
            SetActive();
            return;
            }
        TInt sp1 = iReq.Locate(' ');
        TPtrC8 target = sp1 < 0 ? TPtrC8() : iReq.Mid(sp1 + 1);
        TInt sp2 = target.Locate(' ');
        if (sp2 >= 0) target.Set(target.Left(sp2));
        iBody.Zero();
        GpsReply(iGps, target, iBody);
        iReply.Format(_L8("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"), iBody.Length());
        iReply.Append(iBody);
        iState = EWrite;
        iConn.Write(iReply, iStatus);
        SetActive();
        }
    void DoCancel()
        {
        if (iState == EAccept) iListen.CancelAccept();
        else if (iState == ERead) iConn.CancelRecv();
        else iConn.CancelWrite();
        }
    TGpsState& iGps;
    RSocketServ& iSs;
    RSocket iListen, iConn;
    TBool iConnOpen;
    TInt iState;
    TBuf8<256> iRecv;
    TSockXfrLength iXfr;
    TBuf8<1024> iReq;
    TBuf8<700> iBody;
    TBuf8<900> iReply;
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
    CGpsServer* iServer;
    TBuf8<512> iRecv;
    TSockXfrLength iXfr;
    TBuf8<200> iLine;
    TInt iStep;                 // 1 SDP search, 2 RFCOMM connect, 3 connected
    TInt iGood;                 // the channel that last sent NMEA (tried first)
    TInt iTry;                  // which of the other channels to try next
    TInt iSessionLines;
    };

CGps::~CGps()
    {
    iGps.iStop = NULL;
    delete iServer;
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
    iServer = new (ELeave) CGpsServer(iGps, iSs);
    TRAPD(err, iServer->ConstructL());
    if (err != KErrNone) { TBuf<60> l; l.Format(_L("GPS server on 8124 did not start: %d"), err); AddStatLine(iStats, l); }
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
        else if (err == KErrNotReady && iStep == 3) { info.Format(_L8("channel %d connected but sent nothing for 15 s: trying another channel"), iGps.iChannel); SetState(_L8("error"), info); wait = 1; }
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
    // the channel that sent NMEA last time if it's still offered, else the next one in turn
    TInt channel = -1;
    for (TInt i = 0; i < iSdp->iCount; i++) if (iSdp->iChannels[i] == iGood) channel = iGood;
    if (channel < 0) channel = iSdp->iChannels[iTry % iSdp->iCount];
    TBuf8<100> info;
    TBuf8<40> list;
    for (TInt i = 0; i < iSdp->iCount; i++) list.AppendFormat(i == 0 ? _L8("%d") : _L8(",%d"), iSdp->iChannels[i]);
    info.Format(_L8("connecting to channel %d (offered: %S)"), channel, &list);
    iStep = 2;
    SetState(_L8("connecting"), info);

    err = iBt->iSock.Open(iSs, KBTAddrFamily, KSockStream, KRFCOMM);
    if (err != KErrNone) return err;
    iBt->iOpen = ETrue;
    TBTSockAddr sa;
    sa.SetBTAddr(aAddr);
    sa.SetPort(channel);
    iBt->iSock.Connect(sa, iW->Status());
    err = iW->Wait(KConnectMs, iBt);
    if (err == KErrNone)
        {
        iGps.iLock.Wait();
        iGps.iConnects++;
        iGps.iChannel = channel;
        iGps.iLock.Signal();
        iStep = 3;
        iSessionLines = 0;
        info.Format(_L8("connected (channel %d), waiting for data"), channel);
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
        if (iSessionLines > 0) iGood = channel;
        else
            {
            // connected but silent: not the GPS service; the next channel next time
            if (iGood == channel) iGood = 0;
            iTry++;
            if (err == KErrTimedOut) err = KErrNotReady;
            }
        }
    else iTry++;
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
    iGps.iLock.Signal();
    if (++iSessionLines == 1)
        {
        TBuf8<60> info;
        info.Format(_L8("receiving NMEA (channel %d)"), iGps.iChannel);
        SetState(_L8("connected"), info);
        }
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
