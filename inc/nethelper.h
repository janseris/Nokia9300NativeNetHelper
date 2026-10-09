// Net Helper 9300: a native helper for the Java apps on the Nokia 9300 (Series 80 2.0, Symbian 7.0s).
// It listens on 127.0.0.1:8123. A Java app asks for
//     GET /fetch?u=<percent-encoded URL>
// and the helper fetches that URL over kept-open HTTP/1.1 connections (TLS through the phone's
// CSecureSocket, i.e. the patched SSLADAPTOR.dll) and answers with the server's status, type and body.
// The network work runs in its own thread; the UI only shows the counters.

#ifndef NETHELPER_H
#define NETHELPER_H

#include <eikapp.h>
#include <eikdoc.h>
#include <eikappui.h>
#include <eikbctrl.h>

const TInt KNetHelperPort = 8123;
const TInt KStatLines = 7;

// The GPS, read natively over Bluetooth by its own thread (Java's Bluetooth and HTTP together
// slowed the downloads to seconds and crashed jes-java-comms). The HTTP worker hands it out at /gps.
// Locked by iLock: the HTTP worker writes the wish (address, last request), the GPS thread the rest.
struct TGpsState
    {
    RCriticalSection iLock;
    TBuf8<12> iAddr;            // wanted device, 12 hex digits ("" = none)
    TBool iWanted;
    TTime iLastAsk;             // time of the last /gps request
    TBuf8<120> iGga, iRmc;      // the latest sentences
    TTime iLastData;            // time of the last sentence (0 = none yet)
    TInt iSentences, iConnects, iChannel;
    TBuf8<16> iState;           // idle, searching, connecting, connected, error
    TBuf8<200> iInfo;
    TRequestStatus* iStop;      // the GPS thread's stop request
    };

// Shared between the UI thread and the worker thread (the worker writes, the UI only reads).
struct TNetStats
    {
    TInt iChanged;              // bumped on every change
    TInt iRequests;             // requests from the Java apps
    TInt iFetches;              // /fetch requests done
    TInt iReused;               // ...over a kept-open connection
    TInt iNewConns;             // connections opened
    TInt iErrors;
    TInt iOpenConns;
    TBuf<100> iStatus;
    TBuf<110> iLines[KStatLines];
    TInt iNextLine;
    TRequestStatus* iStop;      // the worker's stop request: the UI completes it on Exit
    TInt iCacheFiles, iCacheKB, iCacheHits, iCacheStored;
    TGpsState iGps;
    };

void AddStatLine(TNetStats& aStats, const TDesC& aLine);
TInt NetWorkerThread(TAny* aStats);
TInt NetGpsThread(TAny* aStats);
void GpsReply(TGpsState& aGps, const TDesC8& aQuery, TDes8& aBody);

class CNetHelperView : public CEikBorderedControl
    {
public:
    static CNetHelperView* NewL(const TRect& aRect, const TNetStats& aStats);
    void Draw(const TRect& aRect) const;
private:
    void Wrapped(CWindowGc& aGc, const CFont& aFont, const TDesC& aText, TPoint& aP, TInt aWidth, TInt aH, TInt aBottom) const;
    CNetHelperView(const TNetStats& aStats) : iStats(aStats) {}
    void ConstructL(const TRect& aRect);
    const TNetStats& iStats;
    };

class CNetHelperAppUi : public CEikAppUi
    {
public:
    void ConstructL();
    ~CNetHelperAppUi();
    void HandleCommandL(TInt aCommand);
    TKeyResponse HandleKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
private:
    static TInt Tick(TAny* aSelf);
    TBool iFull;
    CNetHelperView* iView;
    TNetStats iStats;
    RThread iWorker;
    TBool iWorkerOpen;
    RThread iGpsThread;
    TBool iGpsOpen;
    void StopThread(RThread& aThread, TRequestStatus* aStop);
    CPeriodic* iTimer;
    TInt iSeen;
    };

class CNetHelperDocument : public CEikDocument
    {
public:
    CNetHelperDocument(CEikApplication& aApp) : CEikDocument(aApp) {}
    CEikAppUi* CreateAppUiL() { return new (ELeave) CNetHelperAppUi; }
    };

class CNetHelperApplication : public CEikApplication
    {
public:
    TUid AppDllUid() const;
protected:
    CApaDocument* CreateDocumentL() { return new (ELeave) CNetHelperDocument(*this); }
    };

#endif
