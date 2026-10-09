#include <eikenv.h>
#include <cknenv.h>
#include <ckninfo.h>
#include <eikbtgpc.h>
#include <e32hal.h>
#include <f32file.h>
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
    User::LeaveIfError(iStats.iGps.iLock.CreateLocal());
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
    name.Format(_L("NetHelperGps%u"), User::TickCount());
    if (iGpsThread.Create(name, NetGpsThread, 16384, 16384, 512 * 1024, &iStats) == KErrNone)
        {
        iGpsOpen = ETrue;
        iGpsThread.Resume();
        }
    iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
    iTimer->Start(500000, 500000, TCallBack(Tick, this));
    iView->DrawNow();
    }

// RAM of the whole phone (free now, lowest seen) and the free space on C:
void CNetHelperAppUi::Measure()
    {
    TMemoryInfoV1Buf info;
    if (UserHal::MemoryInfo(info) == KErrNone)
        {
        iStats.iRamTotalKB = info().iTotalRamInBytes / 1024;
        iStats.iRamFreeKB = info().iFreeRamInBytes / 1024;
        if (iStats.iRamMinFreeKB == 0 || iStats.iRamFreeKB < iStats.iRamMinFreeKB) iStats.iRamMinFreeKB = iStats.iRamFreeKB;
        }
    RFs& fs = iEikonEnv->FsSession();
    TVolumeInfo vol;
    if (fs.Volume(vol, EDriveC) == KErrNone)
        {
        TInt64 kb = vol.iFree / TInt64(1024);
        iStats.iDiskFreeKB = kb.GetTInt();
        }
    // every drive with what the file server says it is (RAM drive, flash, memory card, ROM...)
    if (iTicks % 20 == 1)
        {
        TDriveList list;
        if (fs.DriveList(list) == KErrNone)
            {
            iStats.iDrives.Zero();
            for (TInt d = EDriveA; d <= EDriveZ; d++)
                {
                if (!list[d]) continue;
                TDriveInfo info;
                if (fs.Drive(info, d) != KErrNone) continue;
                const TText* kind = _S("?");
                switch (info.iType)
                    {
                    case EMediaRam: kind = _S("RAM"); break;
                    case EMediaFlash: kind = _S("flash"); break;
                    case EMediaRom: kind = _S("ROM"); break;
                    case EMediaHardDisk: kind = (info.iDriveAtt & KDriveAttRemovable) ? _S("card") : _S("disk"); break;
                    case EMediaNotPresent: kind = _S("empty"); break;
                    case EMediaRemote: kind = _S("remote"); break;
                    default: break;
                    }
                TBuf<60> one;
                TVolumeInfo v;
                if (fs.Volume(v, d) == KErrNone)
                    {
                    TInt64 size = v.iSize / TInt64(1024), free = v.iFree / TInt64(1024);
                    one.Format(_L("%c: %s %d KB, free %d KB%s"), 'A' + d, kind, size.GetTInt(), free.GetTInt(),
                        (info.iDriveAtt & KDriveAttSubsted) ? _S(" (substituted)") : _S(""));
                    }
                else one.Format(_L("%c: %s"), 'A' + d, kind);
                if (iStats.iDrives.Length() > 0 && iStats.iDrives.Length() + 3 <= iStats.iDrives.MaxLength()) iStats.iDrives.Append(_L(" | "));
                if (iStats.iDrives.Length() + one.Length() <= iStats.iDrives.MaxLength()) iStats.iDrives.Append(one);
                }
            }
        }
    iStats.iChanged++;
    }

TInt CNetHelperAppUi::Tick(TAny* aSelf)
    {
    CNetHelperAppUi* self = (CNetHelperAppUi*) aSelf;
    if (self->iTicks++ % 4 == 1) self->Measure();
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
    if (iGpsOpen) { StopThread(iGpsThread, iStats.iGps.iStop); iGpsThread.Close(); }
    if (iWorkerOpen) { StopThread(iWorker, iStats.iStop); iWorker.Close(); }
    iStats.iGps.iLock.Close();
    if (iView)
        {
        iEikonEnv->RemoveFromStack(iView);
        delete iView;
        }
    }

// Asks a thread to stop and close its sockets itself (killing it in the middle of a socket, TLS or
// Bluetooth call is what Net Helper 0.2 did on Exit); kills it only if it hangs for 3 s.
void CNetHelperAppUi::StopThread(RThread& aThread, TRequestStatus* aStop)
    {
    if (aThread.ExitType() != EExitPending) return;
    TRequestStatus logon;
    aThread.Logon(logon);
    if (aStop) aThread.RequestComplete(aStop, KErrCancel);
    RTimer timer;
    if (timer.CreateLocal() != KErrNone) { User::WaitForRequest(logon); return; }
    TRequestStatus tick;
    timer.After(tick, 3000000);
    User::WaitForRequest(logon, tick);
    if (logon == KRequestPending)
        {
        aThread.LogonCancel(logon);
        User::WaitForRequest(logon);
        aThread.Kill(KErrNone);
        }
    else
        {
        timer.Cancel();
        User::WaitForRequest(tick);
        }
    timer.Close();
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
            _LIT(KTitle, "Net Helper 9300 0.10");
            _LIT(KText, "Native helper for the Java apps. GET http://127.0.0.1:8123/fetch?u=<URL> fetches the URL over kept-open connections; /gps?addr=<BT address> (also on 127.0.0.1:8124, answered at once) reads the Bluetooth GPS. F: full screen.");
            CCknInfoDialog::RunDlgLD(KTitle, KText);
            }
            break;
        default:
            break;
        }
    }

// F: full screen (the side buttons' labels hidden) and back. Up/down arrows: scroll the text.
TKeyResponse CNetHelperAppUi::HandleKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
    {
    if (aType != EEventKey || !iView) return EKeyWasNotConsumed;
    if (aKeyEvent.iCode == EKeyDownArrow || aKeyEvent.iCode == EKeyUpArrow)
        {
        iView->iScroll += aKeyEvent.iCode == EKeyDownArrow ? 3 : -3;
        if (iView->iScroll < 0) iView->iScroll = 0;
        if (iView->iScroll > 60) iView->iScroll = 60;
        iView->DrawNow();
        return EKeyWasConsumed;
        }
    if (aKeyEvent.iCode != 'f' && aKeyEvent.iCode != 'F') return EKeyWasNotConsumed;
    iFull = !iFull;
    CEikButtonGroupContainer* cba = CEikButtonGroupContainer::Current();
    if (cba) cba->MakeVisible(!iFull);
    iView->SetRect(iFull ? ApplicationRect() : ClientRect());
    iView->DrawNow();
    return EKeyWasConsumed;
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

// Draws aText from aP, wrapped at spaces to aWidth pixels; aP moves below it.
void CNetHelperView::Wrapped(CWindowGc& aGc, const CFont& aFont, const TDesC& aText, TPoint& aP, TInt aWidth, TInt aH, TInt aBottom) const
    {
    TPtrC rest(aText);
    while (rest.Length() > 0 && aP.iY <= aBottom)
        {
        TInt n = aFont.TextCount(rest, aWidth);
        if (n <= 0) n = 1;
        if (n < rest.Length())
            {
            TInt sp = rest.Left(n).LocateReverse(' ');
            if (sp > 0) n = sp + 1;
            }
        TPtrC line = rest.Left(n);
        aGc.DrawText(line, aP);
        aP.iY += aH;
        rest.Set(rest.Mid(n));
        while (rest.Length() > 0 && rest[0] == ' ') rest.Set(rest.Mid(1));
        }
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
    TInt h = font->HeightInPixels() + 5, w = rect.Width() - 20, bottom = rect.iBr.iY;
    TPoint p(rect.iTl.iX + 10, rect.iTl.iY + h + 2 - iScroll * h);
    Wrapped(gc, *font, _L("Net Helper 9300 0.10 - native helper for the Java apps (F: full screen, arrows: scroll)"), p, w, h, bottom);
    Wrapped(gc, *font, iStats.iStatus, p, w, h, bottom);
    TBuf<200> n;
    n.Format(_L("Requests %d | fetched %d, %d on a kept-open connection | connections opened %d, open %d | errors %d"),
        iStats.iRequests, iStats.iFetches, iStats.iReused, iStats.iNewConns, iStats.iOpenConns, iStats.iErrors);
    Wrapped(gc, *font, n, p, w, h, bottom);
    n.Format(_L("RAM: %d KB in all, free %d KB (lowest %d KB) | C: free %d KB"),
        iStats.iRamTotalKB, iStats.iRamFreeKB, iStats.iRamMinFreeKB, iStats.iDiskFreeKB);
    Wrapped(gc, *font, n, p, w, h, bottom);
    Wrapped(gc, *font, iStats.iDrives, p, w, h, bottom);
    n.Format(_L("Tile cache: %d tiles, %d KB of 32 MB | from the cache %d (last read %d ms), stored %d (last save %d ms)"),
        iStats.iCacheFiles, iStats.iCacheKB, iStats.iCacheHits, iStats.iCacheReadMs, iStats.iCacheStored, iStats.iCacheSaveMs);
    Wrapped(gc, *font, n, p, w, h, bottom);
    // the GPS (read under its lock)
    TGpsState& g = ((TNetStats&) iStats).iGps;
    TBuf<260> gl;
    g.iLock.Wait();
    TBuf<200> info;
    info.Copy(g.iInfo.Left(200));
    TBuf<16> st;
    st.Copy(g.iState);
    gl.Format(_L("GPS: %S - %S | sentences %d"), &st, &info, g.iSentences);
    g.iLock.Signal();
    Wrapped(gc, *font, gl, p, w, h, bottom);
    p.iY += 4;
    gc.SetPenColor(TRgb(0x40, 0x40, 0x40));
    // newest first
    for (TInt i = 1; i <= KStatLines && p.iY <= bottom; i++)
        {
        const TDesC& l = iStats.iLines[(iStats.iNextLine - i + 2 * KStatLines) % KStatLines];
        if (l.Length() == 0) break;
        Wrapped(gc, *font, l, p, w, h, bottom);
        }
    gc.DiscardFont();
    }
