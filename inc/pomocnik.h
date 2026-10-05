// Pomocnik 9300: a native helper for the Java apps on the Nokia 9300 (Series 80 2.0, Symbian 7.0s).
// Step 1: an app that listens on 127.0.0.1:8123 and answers every HTTP request with a short text,
// to prove that a Java MIDlet can reach native code on the phone.

#ifndef POMOCNIK_H
#define POMOCNIK_H

#include <eikapp.h>
#include <eikdoc.h>
#include <eikappui.h>
#include <eikbctrl.h>
#include <es_sock.h>
#include <in_sock.h>

const TInt KPomocnikPort = 8123;

class MServerObserver
    {
public:
    virtual void ServerChanged() = 0;
    };

// A tiny HTTP server: accepts one connection at a time, reads the request head, answers, closes.
class CHttpServer : public CActive
    {
public:
    static CHttpServer* NewL(MServerObserver& aObserver);
    ~CHttpServer();
    TInt Requests() const { return iRequests; }
    const TDesC& Status() const { return iStatus2; }
    const TDesC8& LastRequest() const { return iLastLine; }
private:
    CHttpServer(MServerObserver& aObserver);
    void ConstructL();
    void AcceptNext();
    void Fail(const TDesC& aWhat, TInt aErr);
    void RunL();
    void DoCancel();
    enum TState { EAccepting, EReading, EWriting };
    MServerObserver& iObserver;
    RSocketServ iSs;
    RSocket iListen;
    RSocket iConn;
    TBool iConnOpen;
    TState iState;
    TBuf8<512> iRecv;
    TSockXfrLength iRecvLen;
    TBuf8<2048> iRequest;
    TBuf8<512> iReply;
    TBuf8<120> iLastLine;
    TBuf<120> iStatus2;
    TInt iRequests;
    };

class CPomocnikView : public CEikBorderedControl
    {
public:
    static CPomocnikView* NewL(const TRect& aRect, CHttpServer*& aServer);
    void Draw(const TRect& aRect) const;
private:
    CPomocnikView(CHttpServer*& aServer) : iServer(aServer) {}
    void ConstructL(const TRect& aRect);
    CHttpServer*& iServer;
    };

class CPomocnikAppUi : public CEikAppUi, public MServerObserver
    {
public:
    void ConstructL();
    ~CPomocnikAppUi();
    void HandleCommandL(TInt aCommand);
    void ServerChanged();
private:
    CPomocnikView* iView;
    CHttpServer* iServer;
    };

class CPomocnikDocument : public CEikDocument
    {
public:
    CPomocnikDocument(CEikApplication& aApp) : CEikDocument(aApp) {}
    CEikAppUi* CreateAppUiL() { return new (ELeave) CPomocnikAppUi; }
    };

class CPomocnikApplication : public CEikApplication
    {
public:
    TUid AppDllUid() const;
protected:
    CApaDocument* CreateDocumentL() { return new (ELeave) CPomocnikDocument(*this); }
    };

#endif
