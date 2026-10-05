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
    iStats.iStatus.Copy(_L("Starting..."));
    iView = CNetHelperView::NewL(ClientRect(), iStats);
    AddToStackL(iView);
    // the network work runs in its own thread with its own active scheduler, so the
    // blocking-style waits there never stall the UI
    TBuf<40> name;
    name.Format(_L("NetHelperWorker%u"), User::TickCount());
    TInt err = iWorker.Create(name, NetWorkerThread, 16384, 16384, 3 * 1024 * 1024, &iStats);
    if (err == KErrNone)
        {
        iWorkerOpen = ETrue;
        iWorker.Resume();
        }
    else
        {
        iStats.iStatus.Format(_L("Worker thread did not start: error %d"), err);
        }
    iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
    iTimer->Start(500000, 500000, TCallBack(Tick, this));
    iView->DrawNow();
    }

TInt CNetHelperAppUi::Tick(TAny* aSelf)
    {
    CNetHelperAppUi* self = (CNetHelperAppUi*) aSelf;
    if (self->iStats.iChanged != self->iSeen)
        {
        self->iSeen = self->iStats.iChanged;
        if (self->iView) self->iView->DrawNow();
        }
    return 1;
    }

CNetHelperAppUi::~CNetHelperAppUi()
    {
    delete iTimer;
    if (iWorkerOpen)
        {
        iWorker.Kill(KErrNone);         // its sockets are closed by the system
        iWorker.Close();
        }
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
            _LIT(KTitle, "Net Helper 9300 0.2");
            _LIT(KText, "Native helper for the Java apps. GET http://127.0.0.1:8123/fetch?u=<URL> fetches the URL over kept-open connections.");
            CCknInfoDialog::RunDlgLD(KTitle, KText);
            }
            break;
        default:
            break;
        }
    }

// ------------------------------------------------------------------ view

CNetHelperView* CNetHelperView::NewL(const TRect& aRect, const TNetStats& aStats)
    {
    CNetHelperView* self = new (ELeave) CNetHelperView(aStats);
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
    TInt h = font->HeightInPixels() + 5;
    TPoint p(rect.iTl.iX + 10, rect.iTl.iY + h + 2);
    gc.DrawText(_L("Net Helper 9300 0.2 - native helper for the Java apps"), p);
    p.iY += h;
    gc.DrawText(iStats.iStatus, p);
    p.iY += h;
    TBuf<160> n;
    n.Format(_L("Requests %d | fetched %d, %d on a kept-open connection | connections opened %d, open %d | errors %d"),
        iStats.iRequests, iStats.iFetches, iStats.iReused, iStats.iNewConns, iStats.iOpenConns, iStats.iErrors);
    gc.DrawText(n, p);
    p.iY += h + 4;
    gc.SetPenColor(TRgb(0x40, 0x40, 0x40));
    // newest first
    for (TInt i = 1; i <= KStatLines; i++)
        {
        const TDesC& l = iStats.iLines[(iStats.iNextLine - i + 2 * KStatLines) % KStatLines];
        if (l.Length() == 0) break;
        if (p.iY > rect.iBr.iY) break;
        gc.DrawText(l, p);
        p.iY += h;
        }
    gc.DiscardFont();
    }

