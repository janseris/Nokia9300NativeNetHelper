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
    };

void AddStatLine(TNetStats& aStats, const TDesC& aLine);
TInt NetWorkerThread(TAny* aStats);

class CNetHelperView : public CEikBorderedControl
    {
public:
    static CNetHelperView* NewL(const TRect& aRect, const TNetStats& aStats);
    void Draw(const TRect& aRect) const;
private:
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
private:
    static TInt Tick(TAny* aSelf);
    CNetHelperView* iView;
    TNetStats iStats;
    RThread iWorker;
    TBool iWorkerOpen;
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
