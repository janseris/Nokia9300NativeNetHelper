#include <eikenv.h>
#include <cknenv.h>
#include <ckninfo.h>
#include <nethelper.rsg>
#include "nethelper.h"
#include "nethelper.hrh"

static const TUid KUidNetHelper = { 0x0F5A9300 };

// ------------------------------------------------------------------ app framework

GLDEF_C TInt E32Dll(TDllReason) { return KErrNone; }

EXPORT_C CApaApplication* NewApplication() { return new CNetHelperApplication; }

TUid CNetHelperApplication::AppDllUid() const { return KUidNetHelper; }

void CNetHelperAppUi::ConstructL()
    {
    BaseConstructL();
    iView = CNetHelperView::NewL(ClientRect(), iServer);
    AddToStackL(iView);
    TRAPD(err, iServer = CHttpServer::NewL(*this));
    if (err != KErrNone)
        {
        _LIT(KTitle, "Net Helper");
        TBuf<64> msg;
        msg.Format(_L("Server did not start: error %d"), err);
        CCknInfoDialog::RunDlgLD(KTitle, msg);
        }
    iView->DrawNow();
    }

CNetHelperAppUi::~CNetHelperAppUi()
    {
    delete iServer;
    if (iView)
        {
        iEikonEnv->RemoveFromStack(iView);
        delete iView;
        }
    }

void CNetHelperAppUi::HandleCommandL(TInt aCommand)
    {
    switch (aCommand)
        {
        case EEikCmdExit:
            CBaActiveScheduler::Exit();
            break;
        case ENetHelperCmdInfo:
            {
            _LIT(KTitle, "Net Helper 9300");
            _LIT(KText, "Native helper for the Java apps. Step 1: answers on http://127.0.0.1:8123/");
            CCknInfoDialog::RunDlgLD(KTitle, KText);
            }
            break;
        default:
            break;
        }
    }

void CNetHelperAppUi::ServerChanged()
    {
    if (iView) iView->DrawNow();
    }

// ------------------------------------------------------------------ view

CNetHelperView* CNetHelperView::NewL(const TRect& aRect, CHttpServer*& aServer)
    {
    CNetHelperView* self = new (ELeave) CNetHelperView(aServer);
    CleanupStack::PushL(self);
    self->ConstructL(aRect);
    CleanupStack::Pop(self);
    return self;
    }

void CNetHelperView::ConstructL(const TRect& aRect)
    {
    CreateWindowL();
    SetRect(aRect);
    SetBlank();
    SetBorder(TGulBorder::EFlatContainer);
    CknEnv::Skin().SetAppViewType(ESkinAppViewWithCbaNoToolband);
    ActivateL();
    }

void CNetHelperView::Draw(const TRect& aRect) const
    {
    CEikBorderedControl::Draw(aRect);
    CWindowGc& gc = SystemGc();
    TRect rect = Border().InnerRect(Rect());
    gc.SetClippingRect(rect);
    gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
    gc.SetBrushColor(KRgbWhite);
    gc.Clear(rect);
    const CFont* font = iEikonEnv->NormalFont();
    gc.UseFont(font);
    gc.SetPenColor(KRgbBlack);
    TInt h = font->HeightInPixels() + 6;
    TPoint p(rect.iTl.iX + 10, rect.iTl.iY + h + 4);
    gc.DrawText(_L("Net Helper 9300 - native helper for the Java apps"), p);
    p.iY += h;
    if (iServer)
        {
        gc.DrawText(iServer->Status(), p);
        p.iY += h;
        TBuf<64> n;
        n.Format(_L("Requests: %d"), iServer->Requests());
        gc.DrawText(n, p);
        p.iY += h;
        TBuf<130> last;
        last.Copy(iServer->LastRequest());
        if (last.Length() > 0)
            {
            gc.DrawText(_L("Last:"), p);
            p.iY += h;
            gc.DrawText(last, p);
            }
        }
    else
        {
        gc.DrawText(_L("Server not running."), p);
        }
    gc.DiscardFont();
    }

// ------------------------------------------------------------------ server

CHttpServer* CHttpServer::NewL(MServerObserver& aObserver)
    {
    CHttpServer* self = new (ELeave) CHttpServer(aObserver);
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop(self);
    return self;
    }

CHttpServer::CHttpServer(MServerObserver& aObserver) : CActive(EPriorityStandard), iObserver(aObserver)
    {
    CActiveScheduler::Add(this);
    }

void CHttpServer::ConstructL()
    {
    User::LeaveIfError(iSs.Connect());
    User::LeaveIfError(iListen.Open(iSs, KAfInet, KSockStream, KProtocolInetTcp));
    iListen.SetOpt(KSoReuseAddr, KSolInetIp, 1);
    TInetAddr addr(INET_ADDR(127, 0, 0, 1), KNetHelperPort);
    User::LeaveIfError(iListen.Bind(addr));
    User::LeaveIfError(iListen.Listen(4));
    iStatus2.Format(_L("Listening on 127.0.0.1:%d"), KNetHelperPort);
    AcceptNext();
    }

CHttpServer::~CHttpServer()
    {
    Cancel();
    if (iConnOpen) iConn.Close();
    iListen.Close();
    iSs.Close();
    }

void CHttpServer::AcceptNext()
    {
    if (iConnOpen) { iConn.Close(); iConnOpen = EFalse; }
    TInt err = iConn.Open(iSs);
    if (err != KErrNone) { Fail(_L("Open"), err); return; }
    iConnOpen = ETrue;
    iRequest.Zero();
    iState = EAccepting;
    iListen.Accept(iConn, iStatus);
    SetActive();
    }

void CHttpServer::Fail(const TDesC& aWhat, TInt aErr)
    {
    iStatus2.Format(_L("Error: %S %d"), &aWhat, aErr);
    iObserver.ServerChanged();
    }

void CHttpServer::RunL()
    {
    if (iStatus.Int() != KErrNone)
        {
        Fail(iState == EAccepting ? _L("Accept") : iState == EReading ? _L("Read") : _L("Write"), iStatus.Int());
        if (iState == EAccepting) return;      // the listening socket is broken: stop
        AcceptNext();
        return;
        }
    switch (iState)
        {
        case EAccepting:
        case EReading:
            if (iState == EReading)
                {
                if (iRequest.Length() + iRecv.Length() <= iRequest.MaxLength()) iRequest.Append(iRecv);
                if (iRequest.Find(_L8("\r\n\r\n")) >= 0 || iRequest.Length() == iRequest.MaxLength())
                    {
                    TInt eol = iRequest.Find(_L8("\r\n"));
                    iLastLine = iRequest.Left(eol < 0 ? Min(iRequest.Length(), 120) : Min(eol, 120));
                    iRequests++;
                    TBuf8<200> body;
                    body.Format(_L8("Net Helper 9300: hello from native code, request %d\n"), iRequests);
                    iReply.Format(_L8("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"), body.Length());
                    iReply.Append(body);
                    iState = EWriting;
                    iConn.Write(iReply, iStatus);
                    SetActive();
                    iObserver.ServerChanged();
                    return;
                    }
                }
            iState = EReading;
            iConn.RecvOneOrMore(iRecv, 0, iStatus, iRecvLen);
            SetActive();
            break;
        case EWriting:
            AcceptNext();
            break;
        }
    }

void CHttpServer::DoCancel()
    {
    if (iState == EAccepting) iListen.CancelAccept();
    else if (iState == EReading) iConn.CancelRecv();
    else iConn.CancelWrite();
    }
