// Net Helper 9300: the tile cache, files on the phone's disk (C:\Data\NetHelper\tiles\).
//
// Java's record store was the slowest part of Mapy (Mapy 4.16 log: saving one 20 KB tile into the
// 8.5 MB record store took 3.5-13 s and held up the whole Java VM meanwhile). Here one file per tile:
// a hash of the URL (without the API key) as the name, the URL and the content type in the first two
// lines (checked on reading), then the body. Least recently used first out: a hit touches the file,
// and when the total goes over the limit the oldest files go until it's at 90 %.

#include <e32base.h>
#include <f32file.h>
#include "tilecache.h"

_LIT(KDir, "C:\\Data\\NetHelper\\tiles\\");

CTileCache::CTileCache(RFs& aFs) : iFs(aFs) {}

void CTileCache::ConstructL(TInt aLimitBytes)
    {
    iLimit = aLimitBytes;
    TInt err = iFs.MkDirAll(KDir);
    if (err != KErrNone && err != KErrAlreadyExists) User::Leave(err);
    CDir* dir = NULL;
    User::LeaveIfError(iFs.GetDir(KDir, KEntryAttNormal, ESortNone, dir));
    iTotal = 0;
    for (TInt i = 0; i < dir->Count(); i++) iTotal += (*dir)[i].iSize;
    iFiles = dir->Count();
    delete dir;
    }

// The URL as the cache knows it: without "apikey=..." (a new key keeps the tiles, and no key on disk).
void CTileCache::Key(const TDesC8& aUrl, TDes8& aKey)
    {
    aKey.Zero();
    TInt a = aUrl.Find(_L8("apikey="));
    if (a < 0) { aKey.Append(aUrl.Left(aKey.MaxLength())); return; }
    aKey.Append(aUrl.Left(a));
    TPtrC8 rest = aUrl.Mid(a);
    TInt amp = rest.Locate('&');
    if (amp >= 0) aKey.Append(rest.Mid(amp + 1).Left(aKey.MaxLength() - aKey.Length()));
    }

void CTileCache::Name(const TDesC8& aKey, TDes& aName)
    {
    // FNV-1a, 32 bits, plus the length: collisions are caught by the URL inside the file
    TUint32 h = 2166136261u;
    for (TInt i = 0; i < aKey.Length(); i++) { h ^= aKey[i]; h *= 16777619u; }
    aName.Copy(KDir);
    aName.AppendFormat(_L("%08x%03x.t"), h, aKey.Length() & 0xfff);
    }

// The cached body (caller owns it) and its type, or NULL.
HBufC8* CTileCache::Get(const TDesC8& aUrl, TDes8& aType)
    {
    TBuf8<600> key;
    Key(aUrl, key);
    TFileName name;
    Name(key, name);
    RFile f;
    if (f.Open(iFs, name, EFileRead | EFileShareAny) != KErrNone) return NULL;
    TInt size = 0;
    f.Size(size);
    HBufC8* all = HBufC8::New(size);
    if (!all) { f.Close(); return NULL; }
    TPtr8 p = all->Des();
    TInt err = f.Read(p, size);
    f.Close();
    TInt e1 = p.Locate('\n');
    TPtrC8 rest = e1 < 0 ? TPtrC8() : p.Mid(e1 + 1);
    TInt e2 = rest.Locate('\n');
    if (err != KErrNone || e1 < 0 || e2 < 0 || p.Left(e1).Compare(key) != 0) { delete all; return NULL; }
    aType = rest.Left(Min(e2, aType.MaxLength()));
    TInt start = e1 + 1 + e2 + 1;
    p.Delete(0, start);              // the body only (in place, no second buffer)
    TTime now;
    now.HomeTime();
    iFs.SetModified(name, now);      // used: last out
    iHits++;
    return all;
    }

TBool CTileCache::Has(const TDesC8& aUrl)
    {
    TBuf8<600> key;
    Key(aUrl, key);
    TFileName name;
    Name(key, name);
    TEntry e;
    return iFs.Entry(name, e) == KErrNone;
    }

void CTileCache::Put(const TDesC8& aUrl, const TDesC8& aType, const TDesC8& aBody)
    {
    if (aBody.Length() == 0 || aBody.Length() > iLimit / 8) return;
    TBuf8<600> key;
    Key(aUrl, key);
    TFileName name;
    Name(key, name);
    TEntry old;
    TBool existed = iFs.Entry(name, old) == KErrNone;
    RFile f;
    if (f.Replace(iFs, name, EFileWrite) != KErrNone) return;
    TBuf8<700> head;
    head.Append(key.Left(590));
    head.Append('\n');
    head.Append(aType.Left(100));
    head.Append('\n');
    TInt err = f.Write(head);
    if (err == KErrNone) err = f.Write(aBody);
    f.Close();
    if (err != KErrNone) { iFs.Delete(name); if (existed) { iTotal -= old.iSize; iFiles--; } return; }
    if (existed) { iTotal -= old.iSize; iFiles--; }
    iTotal += head.Length() + aBody.Length();
    iFiles++;
    iStored++;
    if (iTotal > iLimit) Evict();
    }

void CTileCache::Evict()
    {
    CDir* dir = NULL;
    if (iFs.GetDir(KDir, KEntryAttNormal, ESortByDate, dir) != KErrNone) return;   // oldest first
    TInt target = iLimit / 10 * 9;
    TFileName name;
    for (TInt i = 0; i < dir->Count() && iTotal > target; i++)
        {
        name.Copy(KDir);
        name.Append((*dir)[i].iName);
        if (iFs.Delete(name) == KErrNone) { iTotal -= (*dir)[i].iSize; iFiles--; }
        }
    delete dir;
    }
