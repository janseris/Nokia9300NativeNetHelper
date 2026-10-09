// Net Helper 9300, the worker thread: a small HTTP server on 127.0.0.1:8123 for the Java apps and
// a pool of kept-open HTTP/1.1 connections to the servers they use.
//
// The code is written in a blocking style: each socket call is issued and then waited for with a
// nested active scheduler (CActiveSchedulerWait), so the TLS code's own active objects keep running.
// One request from a Java app is handled at a time.

#include <e32base.h>
#include <es_sock.h>
#include <in_sock.h>
#include <securesocket.h>
#include <ssl.h>
#include "nethelper.h"

const TInt KMaxBody = 1024 * 1024;
const TInt KMaxConns = 4;
const TInt KIdleSecs = 50;                  // don't reuse a connection idle longer than this
const TInt KConnectMs = 15000, KHandshakeMs = 20000, KIoMs = 20000, KClientMs = 10000;
_LIT8(KReused8, "reused");
_LIT8(KNew8, "new");
_LIT(KReused, "reused");
_LIT(KNew, "new");
_LIT8(KDefaultUa, "NetHelper9300/0.5 (Symbian native helper; Nokia 9300; SymbianOS/7.0s Series80/2.0)");

void AddStatLine(TNetStats& aStats, const TDesC& aLine)
    {
    aStats.iLines[aStats.iNextLine] = aLine.Left(aStats.iLines[0].MaxLength());
    aStats.iNextLine = (aStats.iNextLine + 1) % KStatLines;
    aStats.iChanged++;
    }

#include "wait.h"

// ------------------------------------------------------------------ connections to servers

class CConn : public CBase, public MCancelIo
    {
public:
    ~CConn() { Close(); }
    void CancelIo()
        {
        if (iSecure) iSecure->CancelAll();
        else if (iOpen) iSock.CancelAll();
        }
    void Close()
        {
        if (iSecure) { iSecure->Close(); delete iSecure; iSecure = NULL; }
        if (iOpen) { iSock.Close(); iOpen = EFalse; }
        iBuf.Zero();
        iPos = 0;
        }
    TBuf8<100> iHost;
    TInt iPort;
    TBool iTls;
    RSocket iSock;
    TBool iOpen;
    CSecureSocket* iSecure;
    TInetAddr iAddr;
    TBool iHaveAddr;
    TTime iLastUsed;
    TInt iUses;
    TBuf8<8192> iBuf;           // received, not yet consumed: iBuf.Mid(iPos)
    TInt iPos;
    };

class CClientIo : public CBase, public MCancelIo
    {
public:
    void CancelIo() { iSock.CancelAll(); }
    RSocket iSock;
    };

class CListenIo : public CBase, public MCancelIo
    {
public:
    void CancelIo() { iSock.CancelAccept(); }
    RSocket iSock;
    };

struct TFetchResult
    {
    TInt iCode;
    TBuf8<100> iType;
    HBufC8* iBody;
    TBool iReused;
    TInt iDnsMs, iConnectMs, iTlsMs, iFirstByteMs, iTotalMs;
    TBuf<40> iError;            // where it failed
    };

class CWorker : public CBase
    {
public:
    CWorker(TNetStats& aStats) : iStats(aStats) {}
    ~CWorker();
    void ConstructL();
    void ServeL();
private:
    void HandleClientL();
    void ReplyL(const TDesC8& aHead, const TDesC8& aBody);
    TInt FetchL(const TDesC8& aUrl, const TDesC8& aUa, TFetchResult& aRes);
    TInt FetchOnceL(CConn& aC, const TDesC8& aPath, const TDesC8& aUa, TFetchResult& aRes, TBool& aRetry);
    CConn* ConnFor(const TDesC8& aHost, TInt aPort, TBool aTls);
    TInt OpenL(CConn& aC, TFetchResult& aRes);
    TInt Send(CConn& aC, const TDesC8& aData);
    TInt Fill(CConn& aC);
    TInt ReadLine(CConn& aC, TDes8& aLine);
    TInt ReadBody(CConn& aC, TInt aLen, HBufC8*& aBody);
    void CountOpen();
    static TInt Ms(const TTime& aFrom);

    TNetStats& iStats;
    RSocketServ iSs;
    CListenIo* iListen;
    CClientIo* iClient;
    CWaiter* iW;
    CStopper* iStopper;
    RPointerArray<CConn> iConns;
    TBuf8<4096> iTmp;
    TSockXfrLength iXfr;
    };

static TInt FindNoCase(const TDesC8& aIn, const TDesC8& aWhat)
    {
    TInt n = aWhat.Length();
    for (TInt i = 0; i + n <= aIn.Length(); i++)
        if (aIn.Mid(i, n).CompareF(aWhat) == 0) return i;
    return KErrNotFound;
    }

// The value of header aName (e.g. "content-length") in a block of header lines, trimmed.
static TPtrC8 Header(const TDesC8& aHead, const TDesC8& aName)
    {
    TInt p = 0;
    while (p < aHead.Length())
        {
        TPtrC8 rest = aHead.Mid(p);
        TInt eol = rest.Find(_L8("\r\n"));
        TPtrC8 line = eol < 0 ? rest : rest.Left(eol);
        if (line.Length() > aName.Length() && line[aName.Length()] == ':' && line.Left(aName.Length()).CompareF(aName) == 0)
            {
            TPtrC8 v = line.Mid(aName.Length() + 1);
            while (v.Length() > 0 && v[0] == ' ') v.Set(v.Mid(1));
            while (v.Length() > 0 && v[v.Length() - 1] == ' ') v.Set(v.Left(v.Length() - 1));
            return v;
            }
        if (eol < 0) break;
        p += eol + 2;
        }
    return TPtrC8();
    }

static void PercentDecode(TDes8& aS)
    {
    TInt o = 0;
    for (TInt i = 0; i < aS.Length(); i++)
        {
        TUint8 c = aS[i];
        if (c == '%' && i + 2 < aS.Length())
            {
            TLex8 lex(aS.Mid(i + 1, 2));
            TUint v;
            if (lex.Val(v, EHex) == KErrNone) { aS[o++] = (TUint8) v; i += 2; continue; }
            }
        aS[o++] = c;
        }
    aS.SetLength(o);
    }

TInt CWorker::Ms(const TTime& aFrom)
    {
    TTime now;
    now.HomeTime();
    TInt64 us = now.MicroSecondsFrom(aFrom).Int64();
    us /= 1000;
    return us.GetTInt();
    }

CWorker::~CWorker()
    {
    iStats.iStop = NULL;
    delete iStopper;
    iConns.ResetAndDestroy();
    if (iClient) { iClient->iSock.Close(); delete iClient; }
    if (iListen) { iListen->iSock.Close(); delete iListen; }
    delete iW;
    iSs.Close();
    }

void CWorker::ConstructL()
    {
    iW = new (ELeave) CWaiter;
    iW->ConstructL();
    iStopper = new (ELeave) CStopper(*iW);
    iStats.iStop = iStopper->StatusPtr();
    iListen = new (ELeave) CListenIo;
    iClient = new (ELeave) CClientIo;
    User::LeaveIfError(iSs.Connect());
    User::LeaveIfError(iListen->iSock.Open(iSs, KAfInet, KSockStream, KProtocolInetTcp));
    iListen->iSock.SetOpt(KSoReuseAddr, KSolInetIp, 1);
    TInetAddr addr(INET_ADDR(127, 0, 0, 1), KNetHelperPort);
    User::LeaveIfError(iListen->iSock.Bind(addr));
    User::LeaveIfError(iListen->iSock.Listen(4));
    iStats.iStatus.Format(_L("Listening on 127.0.0.1:%d"), KNetHelperPort);
    iStats.iChanged++;
    }

void CWorker::CountOpen()
    {
    TInt n = 0;
    for (TInt i = 0; i < iConns.Count(); i++) if (iConns[i]->iOpen) n++;
    iStats.iOpenConns = n;
    iStats.iChanged++;
    }

void CWorker::ServeL()
    {
    while (!iW->Stopping())
        {
        User::LeaveIfError(iClient->iSock.Open(iSs));
        iListen->iSock.Accept(iClient->iSock, iW->Status());
        TInt err = iW->Wait(0, iListen);
        if (iW->Stopping()) { iClient->iSock.Close(); break; }
        if (err == KErrNone)
            {
            TRAP(err, HandleClientL());
            if (err != KErrNone)
                {
                TBuf<60> l;
                l.Format(_L("Error %d"), err);
                iStats.iErrors++;
                AddStatLine(iStats, l);
                }
            }
        else
            {
            iStats.iStatus.Format(_L("Accept failed: %d"), err);
            iStats.iChanged++;
            User::After(1000000);
            }
        iClient->iSock.Close();
        }
    }

void CWorker::ReplyL(const TDesC8& aHead, const TDesC8& aBody)
    {
    iClient->iSock.Write(aHead, iW->Status());
    TInt err = iW->Wait(KClientMs, iClient);
    if (err == KErrNone && aBody.Length() > 0)
        {
        iClient->iSock.Write(aBody, iW->Status());
        err = iW->Wait(KClientMs + aBody.Length() / 50, iClient);
        }
    User::LeaveIfError(err);
    }

void CWorker::HandleClientL()
    {
    // the request head from the Java app
    HBufC8* reqBuf = HBufC8::NewLC(4096);
    TPtr8 req = reqBuf->Des();
    while (req.Find(_L8("\r\n\r\n")) < 0)
        {
        iClient->iSock.RecvOneOrMore(iTmp, 0, iW->Status(), iXfr);
        TInt err = iW->Wait(KClientMs, iClient);
        if (err != KErrNone && iTmp.Length() == 0) User::Leave(err);
        if (req.Length() + iTmp.Length() > req.MaxLength()) break;
        req.Append(iTmp);
        if (err != KErrNone) break;
        }
    iStats.iRequests++;
    TInt sp1 = req.Locate(' ');
    TPtrC8 target = sp1 < 0 ? TPtrC8() : req.Mid(sp1 + 1);
    TInt sp2 = target.Locate(' ');
    if (sp2 >= 0) target.Set(target.Left(sp2));

    TBuf8<300> head;
    if (target.Left(4).Compare(_L8("/gps")) == 0)
        {
        // the GPS (read by the GPS thread): /gps?addr=<12 hex digits> keeps it wanted, /gps?stop=1 lets it go
        TGpsState& g = iStats.iGps;
        TPtrC8 q = target.Mid(4);
        TInt a = q.Find(_L8("addr="));
        TTime now;
        now.HomeTime();
        HBufC8* bodyBuf = HBufC8::NewLC(600);
        TPtr8 body = bodyBuf->Des();
        g.iLock.Wait();
        if (q.Find(_L8("stop=1")) >= 0) g.iWanted = EFalse;
        else if (a >= 0 && q.Length() >= a + 5 + 12)
            {
            g.iAddr = q.Mid(a + 5, 12);
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
        body.AppendFormat(_L8("state=%S\ninfo=%S\nage=%d\nsentences=%d\nchannel=%d\nconnects=%d\n"),
            &g.iState, &g.iInfo, age, g.iSentences, g.iChannel, g.iConnects);
        if (g.iGga.Length() > 0) { body.Append(g.iGga); body.Append(_L8("\n")); }
        if (g.iRmc.Length() > 0) { body.Append(g.iRmc); body.Append(_L8("\n")); }
        g.iLock.Signal();
        head.Format(_L8("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"), body.Length());
        ReplyL(head, body);
        CleanupStack::PopAndDestroy(2, reqBuf);     // body, req
        return;
        }
    if (target.Left(9).Compare(_L8("/fetch?u=")) != 0)
        {
        // anything else: a short hello (step 1's test still works)
        TBuf8<200> body;
        body.Format(_L8("Net Helper 9300 0.5: hello from native code, request %d. Use /fetch?u=<URL>\n"), iStats.iRequests);
        head.Format(_L8("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"), body.Length());
        AddStatLine(iStats, _L("hello"));
        ReplyL(head, body);
        CleanupStack::PopAndDestroy(reqBuf);
        return;
        }

    HBufC8* urlBuf = target.Mid(9).AllocLC();
    TPtr8 url = urlBuf->Des();
    TInt amp = url.Locate('&');
    if (amp >= 0) url.SetLength(amp);
    PercentDecode(url);
    TPtrC8 ua = Header(req, _L8("X-Ua"));

    TFetchResult res;
    res.iCode = 0;
    res.iBody = NULL;
    res.iReused = EFalse;
    res.iDnsMs = res.iConnectMs = res.iTlsMs = res.iFirstByteMs = res.iTotalMs = -1;
    TTime t0;
    t0.HomeTime();
    TInt err = KErrNone;
    TRAPD(leave, err = FetchL(url, ua.Length() > 0 ? ua : KDefaultUa(), res));
    if (leave != KErrNone) err = leave;
    res.iTotalMs = Ms(t0);
    CleanupStack::PushL(res.iBody);
    iStats.iFetches++;

    // host for the stats line
    TPtrC8 host = url;
    TInt ss = host.Find(_L8("://"));
    if (ss >= 0) host.Set(host.Mid(ss + 3));
    TInt sl = host.Locate('/');
    if (sl >= 0) host.Set(host.Left(sl));
    TBuf<40> host16;
    host16.Copy(host.Left(40));

    TBuf<110> line;
    TBuf8<120> info;
    info.Format(_L8("conn=%S dns=%d connect=%d tls=%d first-byte=%d total=%d"),
        res.iReused ? &KReused8() : &KNew8(), res.iDnsMs, res.iConnectMs, res.iTlsMs, res.iFirstByteMs, res.iTotalMs);
    if (err == KErrNone)
        {
        if (res.iReused) iStats.iReused++;
        TPtrC8 body = res.iBody ? TPtrC8(*res.iBody) : TPtrC8();
        head.Format(_L8("HTTP/1.0 %d Upstream\r\nContent-Type: %S\r\nContent-Length: %d\r\nX-Helper: %S\r\nConnection: close\r\n\r\n"),
            res.iCode, &res.iType, body.Length(), &info);
        line.Format(_L("%d %S %d B %S %d ms"), res.iCode, &host16, body.Length(), res.iReused ? &KReused() : &KNew(), res.iTotalMs);
        AddStatLine(iStats, line);
        ReplyL(head, body);
        }
    else
        {
        iStats.iErrors++;
        TBuf8<120> body;
        TBuf8<40> where;
        where.Copy(res.iError);
        body.Format(_L8("Net Helper: %S failed: error %d\n"), &where, err);
        head.Format(_L8("HTTP/1.0 502 Helper error\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nX-Helper: %S error=%d\r\nX-Helper-Error: %d %S\r\nConnection: close\r\n\r\n"),
            body.Length(), &info, err, err, &where);
        line.Format(_L("ERR %d %S: %S"), err, &res.iError, &host16);
        AddStatLine(iStats, line);
        ReplyL(head, body);
        }
    CleanupStack::PopAndDestroy(3, reqBuf);     // body, url, req
    }

CConn* CWorker::ConnFor(const TDesC8& aHost, TInt aPort, TBool aTls)
    {
    TTime now;
    now.HomeTime();
    CConn* match = NULL;
    for (TInt i = 0; i < iConns.Count(); i++)
        {
        CConn* c = iConns[i];
        if (c->iPort == aPort && c->iTls == aTls && c->iHost.CompareF(aHost) == 0) { match = c; break; }
        }
    if (match)
        {
        TTimeIntervalSeconds idle;
        if (match->iOpen && (now.SecondsFrom(match->iLastUsed, idle) != KErrNone || idle.Int() > KIdleSecs)) match->Close();
        return match;
        }
    if (iConns.Count() >= KMaxConns)
        {
        // drop the least recently used one
        TInt lru = 0;
        for (TInt i = 1; i < iConns.Count(); i++) if (iConns[i]->iLastUsed < iConns[lru]->iLastUsed) lru = i;
        delete iConns[lru];
        iConns.Remove(lru);
        }
    CConn* c = new CConn;
    if (!c) return NULL;
    c->iHost = aHost.Left(c->iHost.MaxLength());
    c->iPort = aPort;
    c->iTls = aTls;
    c->iLastUsed = now;
    if (iConns.Append(c) != KErrNone) { delete c; return NULL; }
    return c;
    }

TInt CWorker::OpenL(CConn& aC, TFetchResult& aRes)
    {
    TTime t;
    TInt err;
    if (!aC.iHaveAddr)
        {
        t.HomeTime();
        RHostResolver r;
        err = r.Open(iSs, KAfInet, KProtocolInetUdp);
        if (err != KErrNone) { aRes.iError.Copy(_L("DNS open")); return err; }
        TNameEntry entry;
        TBuf<100> host16;
        host16.Copy(aC.iHost);
        r.GetByName(host16, entry, iW->Status());
        err = iW->Wait(0, NULL);
        r.Close();
        aRes.iDnsMs = Ms(t);
        if (err != KErrNone) { aRes.iError.Copy(_L("DNS")); return err; }
        aC.iAddr = TInetAddr::Cast(entry().iAddr);
        aC.iHaveAddr = ETrue;
        }
    aC.iAddr.SetPort(aC.iPort);
    t.HomeTime();
    err = aC.iSock.Open(iSs, KAfInet, KSockStream, KProtocolInetTcp);
    if (err != KErrNone) { aRes.iError.Copy(_L("socket")); return err; }
    aC.iOpen = ETrue;
    aC.iSock.Connect(aC.iAddr, iW->Status());
    err = iW->Wait(KConnectMs, &aC);
    aRes.iConnectMs = Ms(t);
    if (err != KErrNone) { aRes.iError.Copy(_L("connect")); aC.iHaveAddr = EFalse; aC.Close(); return err; }
    iStats.iNewConns++;
    if (aC.iTls)
        {
        t.HomeTime();
        TRAP(err, aC.iSecure = CSecureSocket::NewL(aC.iSock, _L("TLS1.0")));
        if (err != KErrNone) { aRes.iError.Copy(_L("TLS create")); aC.Close(); return err; }
        aC.iSecure->SetDialogMode(EDialogModeUnattended);
        aC.iSecure->SetOpt(KSoSSLDomainName, KSolInetSSL, aC.iHost);     // SNI
        aC.iSecure->StartClientHandshake(iW->Status());
        err = iW->Wait(KHandshakeMs, &aC);
        aRes.iTlsMs = Ms(t);
        if (err != KErrNone) { aRes.iError.Copy(_L("TLS handshake")); aC.Close(); return err; }
        }
    aC.iUses = 0;
    aC.iBuf.Zero();
    aC.iPos = 0;
    CountOpen();
    return KErrNone;
    }

TInt CWorker::Send(CConn& aC, const TDesC8& aData)
    {
    if (aC.iSecure) aC.iSecure->Send(aData, iW->Status());
    else aC.iSock.Write(aData, iW->Status());
    return iW->Wait(KIoMs, &aC);
    }

// Receives more bytes into aC.iBuf (first dropping what's been consumed). KErrEof when closed.
TInt CWorker::Fill(CConn& aC)
    {
    if (aC.iPos > 0)
        {
        aC.iBuf.Delete(0, aC.iPos);
        aC.iPos = 0;
        }
    TInt room = aC.iBuf.MaxLength() - aC.iBuf.Length();
    if (room <= 0) return KErrOverflow;
    TPtr8 into((TUint8*) iTmp.Ptr(), 0, Min(room, iTmp.MaxLength()));
    if (aC.iSecure) aC.iSecure->RecvOneOrMore(into, iW->Status(), iXfr);
    else aC.iSock.RecvOneOrMore(into, 0, iW->Status(), iXfr);
    TInt err = iW->Wait(KIoMs, &aC);
    if (into.Length() > 0) aC.iBuf.Append(into);
    if (err == KErrNone && into.Length() == 0) err = KErrEof;
    if (into.Length() > 0) return KErrNone;
    return err;
    }

TInt CWorker::ReadLine(CConn& aC, TDes8& aLine)
    {
    for (;;)
        {
        TPtrC8 rest = aC.iBuf.Mid(aC.iPos);
        TInt eol = rest.Find(_L8("\r\n"));
        if (eol >= 0)
            {
            aLine = rest.Left(Min(eol, aLine.MaxLength()));
            aC.iPos += eol + 2;
            return KErrNone;
            }
        TInt err = Fill(aC);
        if (err != KErrNone) return err;
        }
    }

TInt CWorker::ReadBody(CConn& aC, TInt aLen, HBufC8*& aBody)
    {
    if (aBody == NULL) { aBody = HBufC8::New(Max(aLen, 1024)); if (!aBody) return KErrNoMemory; }
    if (aBody->Des().MaxLength() < aBody->Length() + aLen)
        {
        HBufC8* b = aBody->ReAlloc(aBody->Length() + aLen);
        if (!b) return KErrNoMemory;
        aBody = b;
        }
    TPtr8 dst = aBody->Des();
    TInt left = aLen;
    while (left > 0)
        {
        TInt have = aC.iBuf.Length() - aC.iPos;
        if (have == 0)
            {
            TInt err = Fill(aC);
            if (err != KErrNone) return err;
            continue;
            }
        TInt k = Min(have, left);
        dst.Append(aC.iBuf.Mid(aC.iPos, k));
        aC.iPos += k;
        left -= k;
        }
    return KErrNone;
    }

TInt CWorker::FetchL(const TDesC8& aUrl, const TDesC8& aUa, TFetchResult& aRes)
    {
    TBool tls;
    TPtrC8 rest;
    if (aUrl.Left(7).CompareF(_L8("http://")) == 0) { tls = EFalse; rest.Set(aUrl.Mid(7)); }
    else if (aUrl.Left(8).CompareF(_L8("https://")) == 0) { tls = ETrue; rest.Set(aUrl.Mid(8)); }
    else { aRes.iError.Copy(_L("URL")); return KErrArgument; }
    TInt slash = rest.Locate('/');
    TPtrC8 hostPort = slash < 0 ? rest : rest.Left(slash);
    TPtrC8 path = slash < 0 ? TPtrC8(_L8("/")) : rest.Mid(slash);
    TInt port = tls ? 443 : 80;
    TPtrC8 host = hostPort;
    TInt colon = hostPort.Locate(':');
    if (colon >= 0)
        {
        host.Set(hostPort.Left(colon));
        TLex8 lex(hostPort.Mid(colon + 1));
        if (lex.Val(port) != KErrNone) { aRes.iError.Copy(_L("URL port")); return KErrArgument; }
        }
    if (host.Length() == 0 || host.Length() > 100) { aRes.iError.Copy(_L("URL host")); return KErrArgument; }

    CConn* c = ConnFor(host, port, tls);
    if (!c) { aRes.iError.Copy(_L("memory")); return KErrNoMemory; }
    for (TInt attempt = 0; attempt < 2; attempt++)
        {
        TBool retry = EFalse;
        TInt err = FetchOnceL(*c, path, aUa, aRes, retry);
        if (err == KErrNone || !retry) return err;
        // a kept-open connection the server had closed meanwhile: once more on a new one
        c->Close();
        CountOpen();
        delete aRes.iBody;
        aRes.iBody = NULL;
        }
    return KErrGeneral;
    }

TInt CWorker::FetchOnceL(CConn& aC, const TDesC8& aPath, const TDesC8& aUa, TFetchResult& aRes, TBool& aRetry)
    {
    TInt err;
    TBool close = EFalse;
    aRes.iReused = aC.iOpen;
    if (!aC.iOpen)
        {
        err = OpenL(aC, aRes);
        if (err != KErrNone) return err;
        }
    HBufC8* reqBuf = HBufC8::NewLC(aPath.Length() + aC.iHost.Length() + aUa.Length() + 160);
    TPtr8 req = reqBuf->Des();
    req.Append(_L8("GET "));
    req.Append(aPath);
    req.Append(_L8(" HTTP/1.1\r\nHost: "));
    req.Append(aC.iHost);
    if (aC.iPort != (aC.iTls ? 443 : 80)) req.AppendFormat(_L8(":%d"), aC.iPort);
    req.Append(_L8("\r\nUser-Agent: "));
    req.Append(aUa);
    req.Append(_L8("\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n"));
    TTime t;
    t.HomeTime();
    err = Send(aC, req);
    CleanupStack::PopAndDestroy(reqBuf);
    if (err != KErrNone) { aRetry = aRes.iReused; aRes.iError.Copy(_L("send")); aC.Close(); CountOpen(); return err; }

    // status line (skipping any 100 Continue)
    TBuf8<200> line;
    TInt code = 0;
    for (;;)
        {
        err = ReadLine(aC, line);
        if (err != KErrNone)
            {
            aRetry = aRes.iReused;
            aRes.iError.Copy(_L("response"));
            aC.Close();
            CountOpen();
            return err;
            }
        if (aRes.iFirstByteMs < 0) aRes.iFirstByteMs = Ms(t);
        if (line.Length() < 12 || line.Left(5).Compare(_L8("HTTP/")) != 0) { aRes.iError.Copy(_L("status line")); aC.Close(); CountOpen(); return KErrCorrupt; }
        TLex8 lex(line.Mid(9, 3));
        lex.Val(code);
        // headers
        HBufC8* headBuf = HBufC8::NewLC(4096);
        TPtr8 head = headBuf->Des();
        TBuf8<512> h;
        for (;;)
            {
            err = ReadLine(aC, h);
            if (err != KErrNone) { aRes.iError.Copy(_L("headers")); aC.Close(); CountOpen(); CleanupStack::PopAndDestroy(headBuf); return err; }
            if (h.Length() == 0) break;
            if (head.Length() + h.Length() + 2 <= head.MaxLength()) { head.Append(h); head.Append(_L8("\r\n")); }
            }
        if (code >= 100 && code < 200) { CleanupStack::PopAndDestroy(headBuf); continue; }

        aRes.iCode = code;
        aRes.iType = Header(head, _L8("Content-Type")).Left(aRes.iType.MaxLength());
        if (aRes.iType.Length() == 0) aRes.iType.Copy(_L8("application/octet-stream"));
        close = FindNoCase(Header(head, _L8("Connection")), _L8("close")) >= 0
            || (line.Left(8).Compare(_L8("HTTP/1.0")) == 0 && FindNoCase(Header(head, _L8("Connection")), _L8("keep-alive")) < 0);
        TBool chunked = FindNoCase(Header(head, _L8("Transfer-Encoding")), _L8("chunked")) >= 0;
        TPtrC8 lenText = Header(head, _L8("Content-Length"));
        TInt len = -1;
        if (lenText.Length() > 0) { TLex8 l2(lenText); if (l2.Val(len) != KErrNone) len = -1; }
        CleanupStack::PopAndDestroy(headBuf);

        if (code == 204 || code == 304) len = 0;
        if (chunked)
            {
            for (;;)
                {
                err = ReadLine(aC, line);
                if (err != KErrNone) break;
                TInt semi = line.Locate(';');
                TPtrC8 hex = semi < 0 ? TPtrC8(line) : line.Left(semi);
                TLex8 l3(hex);
                TUint n = 0;
                if (l3.Val(n, EHex) != KErrNone) { err = KErrCorrupt; break; }
                if (n == 0)
                    {
                    while ((err = ReadLine(aC, line)) == KErrNone && line.Length() > 0) {}
                    break;
                    }
                if ((aRes.iBody ? aRes.iBody->Length() : 0) + (TInt) n > KMaxBody) { err = KErrTooBig; break; }
                err = ReadBody(aC, n, aRes.iBody);
                if (err != KErrNone) break;
                err = ReadLine(aC, line);
                if (err != KErrNone) break;
                }
            }
        else if (len >= 0)
            {
            if (len > KMaxBody) err = KErrTooBig;
            else err = ReadBody(aC, len, aRes.iBody);
            }
        else
            {
            // no length: the body ends when the server closes
            close = ETrue;
            for (;;)
                {
                TInt have = aC.iBuf.Length() - aC.iPos;
                if (have > 0)
                    {
                    if ((aRes.iBody ? aRes.iBody->Length() : 0) + have > KMaxBody) { err = KErrTooBig; break; }
                    err = ReadBody(aC, have, aRes.iBody);
                    if (err != KErrNone) break;
                    }
                err = Fill(aC);
                if (err == KErrEof) { err = KErrNone; break; }
                if (err != KErrNone) break;
                }
            }
        if (err != KErrNone) { aRes.iError.Copy(_L("body")); aC.Close(); CountOpen(); return err; }
        if (!aRes.iBody) aRes.iBody = HBufC8::NewL(0);
        break;
        }
    TTime now;
    now.HomeTime();
    aC.iLastUsed = now;
    aC.iUses++;
    if (close) { aC.Close(); CountOpen(); }
    return KErrNone;
    }

// ------------------------------------------------------------------ thread

LOCAL_C void RunWorkerL(TNetStats& aStats)
    {
    CWorker* w = new (ELeave) CWorker(aStats);
    CleanupStack::PushL(w);
    w->ConstructL();
    w->ServeL();
    CleanupStack::PopAndDestroy(w);
    }

TInt NetWorkerThread(TAny* aStats)
    {
    TNetStats& stats = *(TNetStats*) aStats;
    CTrapCleanup* cleanup = CTrapCleanup::New();
    CActiveScheduler* as = new CActiveScheduler;
    if (!cleanup || !as) return KErrNoMemory;
    CActiveScheduler::Install(as);
    TRAPD(err, RunWorkerL(stats));
    stats.iStatus.Format(_L("Server stopped: error %d"), err);
    stats.iChanged++;
    delete as;
    delete cleanup;
    return err;
    }
