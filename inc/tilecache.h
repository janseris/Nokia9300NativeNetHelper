// Net Helper 9300: the tile cache on disk (see tilecache.cpp).

#ifndef TILECACHE_H
#define TILECACHE_H

#include <e32base.h>
#include <f32file.h>

class CTileCache : public CBase
    {
public:
    CTileCache(RFs& aFs);
    void ConstructL(TInt aLimitBytes);
    HBufC8* Get(const TDesC8& aUrl, TDes8& aType);
    TBool Has(const TDesC8& aUrl);
    void Put(const TDesC8& aUrl, const TDesC8& aType, const TDesC8& aBody);
    TInt iTotal, iFiles, iHits, iStored, iLimit;
private:
    static void Key(const TDesC8& aUrl, TDes8& aKey);
    static void Name(const TDesC8& aKey, TDes& aName);
    void Evict();
    RFs& iFs;
    };

#endif
