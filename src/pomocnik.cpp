#include <eikenv.h>
#include <cknenv.h>
#include <ckninfo.h>
#include <pomocnik.rsg>
#include "pomocnik.h"
#include "pomocnik.hrh"

static const TUid KUidPomocnik = { 0x0F5A9300 };

// ------------------------------------------------------------------ app framework

GLDEF_C TInt E32Dll(TDllReason) { return KErrNone; }

EXPORT_C CApaApplication* NewApplication() { return new CPomocnikApplication; }

TUid CPomocnikApplication::AppDllUid() const { return KUidPomocnik; }

void CPomocnikAppUi::ConstructL()
    {
    BaseConstructL();
    iView = CPomocnikView::NewL(ClientRect(), iServer);
    AddToStackL(iView);
    TRAPD(err, iServer = CHttpServer::NewL(*this));
    if (err != KErrNone)
        {
        _LIT(KTitle, "Pomocnik");
        TBuf<64> msg;
        msg.Format(_L("Server se nespustil: chyba %d"), err);
        CCknInfoDialog::RunDlgLD(KTitle, msg);
        }
    iView->DrawNow();
    }

CPomocnikAppUi::~CPomocnikAppUi()
    {
    delete iServer;
    if (iView)
        {
        iEikonEnv->RemoveFromStack(iView);
        delete iView;
        }
    }

void CPomocnikAppUi::HandleCommandL(TInt aCommand)
    {
    switch (aCommand)
        {
        case EEikCmdExit:
            CBaActiveScheduler::Exit();
            break;
        case EPomocnikCmdInfo:
            {
            _LIT(KTitle, "Pomocnik 9300");
            _LIT(KText, "Nativni pomocnik pro Java aplikace. Krok 1: odpovida na http://127.0.0.1:8123/");
            CCknInfoDialog::RunDlgLD(KTitle, KText);
            }
            break;
        default:
            break;
        }
    }

void CPomocnikAppUi::ServerChanged()
    {
    if (iView) iView->DrawNow();
    }

// ------------------------------------------------------------------ view

CPomocnikView* CPomocnikView::NewL(const TRect& aRect, CHttpServer*& aServer)
    {
    CPomocnikView* self = new (ELeave) CPomocnikView(aServer);
    CleanupStack::PushL(self);
    self->ConstructL(aRect);
    CleanupStack::Pop(self);
    return self;
    }

void CPomocnikView::ConstructL(const TRect& aRect)
    {
    CreateWindowL();
    SetRect(aRect);
    SetBlank();
    SetBorder(TGulBorder::EFlatContainer);
    CknEnv::Skin().SetAppViewType(ESkinAppViewWithCbaNoToolband);
    ActivateL();
    }

void CPomocnikView::Draw(const TRect& aRect) const
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
    gc.DrawText(_L("Pomocnik 9300 - nativni pomocnik pro Java aplikace"), p);
    p.iY += h;
    if (iServer)
        {
        gc.DrawText(iServer->Status(), p);
        p.iY += h;
        TBuf<64> n;
        n.Format(_L("Pozadavku: %d"), iServer->Requests());
        gc.DrawText(n, p);
        p.iY += h;
        TBuf<130> last;
        last.Copy(iServer->LastRequest());
        if (last.Length() > 0)
            {
            gc.DrawText(_L("Posledni:"), p);
            p.iY += h;
            gc.DrawText(last, p);
            }
        }
    else
        {
        gc.DrawText(_L("Server nebezi."), p);
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
    TInetAddr addr(INET_ADDR(127, 0, 0, 1), KPomocnikPort);
    User::LeaveIfError(iListen.Bind(addr));
    User::LeaveIfError(iListen.Listen(4));
    iStatus2.Format(_L("Nasloucham na 127.0.0.1:%d"), KPomocnikPort);
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
    iStatus2.Format(_L("Chyba: %S %d"), &aWhat, aErr);
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
                    body.Format(_L8("Pomocnik 9300: ahoj z nativniho kodu, pozadavek %d\n"), iRequests);
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
