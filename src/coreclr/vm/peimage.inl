// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// --------------------------------------------------------------------------------
// PEImage.inl
//

// --------------------------------------------------------------------------------

#ifndef PEIMAGE_INL_
#define PEIMAGE_INL_

#include "peimage.h"
#include "../dlls/mscorrc/resource.h"

inline ULONG PEImage::AddRef()
{
    CONTRACT(ULONG)
    {
        PRECONDITION(m_refCount>0 && m_refCount < COUNT_T_MAX);
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACT_END;

    RETURN (static_cast<ULONG>(InterlockedIncrement(&m_refCount)));
}

inline const SString &PEImage::GetPath()
{
    LIMITED_METHOD_DAC_CONTRACT;

    return m_path;
}

inline const SString& PEImage::GetPathToLoad()
{
    LIMITED_METHOD_DAC_CONTRACT;

    return IsInBundle() ? m_probeExtensionResult.BundleLocation.Path() : m_path;
}

inline void* PEImage::GetExternalData(INT64* size)
{
    LIMITED_METHOD_CONTRACT;

    _ASSERTE(size != nullptr);
    if (!IsExternalData())
    {
        *size = 0;
        return nullptr;
    }

    *size = m_probeExtensionResult.ExternalData.Size;
    return m_probeExtensionResult.ExternalData.Data;
}

inline INT64 PEImage::GetOffset() const
{
    LIMITED_METHOD_CONTRACT;

    return IsInBundle() ? m_probeExtensionResult.BundleLocation.Offset : 0;
}

inline BOOL PEImage::IsInBundle() const
{
    LIMITED_METHOD_CONTRACT;

    return m_probeExtensionResult.Type == ProbeExtensionResult::Type::Bundle;
}

inline BOOL PEImage::IsExternalData() const
{
    LIMITED_METHOD_CONTRACT;

    return m_probeExtensionResult.Type == ProbeExtensionResult::Type::External;
}

inline INT64 PEImage::GetSize() const
{
    if (IsInBundle())
        return m_probeExtensionResult.BundleLocation.Size;

    if (IsExternalData())
        return m_probeExtensionResult.ExternalData.Size;

    // Size is not specified
    return 0;
}

inline BOOL PEImage::IsCompressed(INT64* uncompressedSize) const
{
    LIMITED_METHOD_CONTRACT;

    if (uncompressedSize != NULL)
        *uncompressedSize = IsInBundle() ? m_probeExtensionResult.BundleLocation.UncompressedSize : 0;

    return IsInBundle() && m_probeExtensionResult.BundleLocation.UncompressedSize != 0;
}

inline void PEImage::SetModuleFileNameHintForDAC()
{
    LIMITED_METHOD_DAC_CONTRACT;

    // Grab module name only for triage dumps where full paths are excluded
    // because may contain PII data.
    // m_sModuleFileNameHintUsedByDac will just point to module name starting character.
    const WCHAR* pStartPath = m_path.GetUnicode();
    COUNT_T nChars = m_path.GetCount();
    if (pStartPath != NULL && nChars > 0 && nChars <= MAX_PATH)
    {
        const WCHAR* pChar = pStartPath + nChars;
        nChars = 0;
        while ((pChar >= pStartPath) && (*pChar != DIRECTORY_SEPARATOR_CHAR_W))
        {
            pChar--;
            nChars++;
        }
        pChar++;
        m_sModuleFileNameHintUsedByDac.SetPreallocated(pChar, nChars);
    }
}

inline const SString &PEImage::GetModuleFileNameHintForDAC()
{
    LIMITED_METHOD_CONTRACT;

    return m_sModuleFileNameHintUsedByDac;
}

inline BOOL PEImage::IsFile()
{
    WRAPPER_NO_CONTRACT;
    return !IsExternalData() && !GetPathToLoad().IsEmpty();
}

//
// GetExistingLayout - get an layout corresponding to the specified mask, or null if none.
// Does not take any locks or call AddRef.
//
// Arguments:
//    imageLayoutMask - bits from PEImageLayout specifying which layouts the caller would be
//                      interested in getting
//
// Return value:
//    a PEImageLayout of a type matching one of the bits specified in the mask, or NULL if
//    none exists yet.  Does not call AddRef on the returned value.
//
inline PTR_PEImageLayout PEImage::GetExistingLayoutInternal(DWORD imageLayoutMask)
{
    LIMITED_METHOD_CONTRACT;
    SUPPORTS_DAC;

    PTR_PEImageLayout pRetVal = NULL;

    if (imageLayoutMask&PEImageLayout::LAYOUT_LOADED)
        pRetVal=m_pLayouts[IMAGE_LOADED];
    if (pRetVal==NULL && (imageLayoutMask & PEImageLayout::LAYOUT_FLAT))
        pRetVal=m_pLayouts[IMAGE_FLAT];

    return pRetVal;
}


inline BOOL PEImage::HasLoadedLayout()
{
    LIMITED_METHOD_CONTRACT;
    SUPPORTS_DAC;
    return m_pLayouts[IMAGE_LOADED]!=NULL;
}

inline PTR_PEImageLayout PEImage::GetLoadedLayout()
{
#ifdef PEIMAGE_FLAT_LAYOUT_ONLY
    return GetFlatLayout();
#endif
    LIMITED_METHOD_CONTRACT;
    SUPPORTS_DAC;

    _ASSERTE(m_pLayouts[IMAGE_LOADED] != NULL);
    return m_pLayouts[IMAGE_LOADED];
}

inline PTR_PEImageLayout PEImage::GetFlatLayout()
{
    LIMITED_METHOD_CONTRACT;
    SUPPORTS_DAC;

    _ASSERTE(m_pLayouts[IMAGE_FLAT] != NULL);
    return m_pLayouts[IMAGE_FLAT];
}

inline BOOL PEImage::IsOpened()
{
    LIMITED_METHOD_CONTRACT;
    return m_pLayouts[IMAGE_LOADED]!=NULL || m_pLayouts[IMAGE_FLAT] !=NULL;
}


inline BOOL PEImage::IsReferenceAssembly()
{
    CONTRACTL
    {
        PRECONDITION(HasCorHeader());
    }
    CONTRACTL_END;

    IMDInternalImport* mdImport = this->GetMDImport();
    HRESULT hr = mdImport->GetCustomAttributeByName(TokenFromRid(1, mdtAssembly),
                                           g_ReferenceAssemblyAttribute,
                                           NULL,
                                           NULL);
    IfFailThrow(hr);
    if (hr == S_OK) {
        return TRUE;
    }
    _ASSERTE(hr == S_FALSE);
    return FALSE;
}


inline BOOL PEImage::HasNTHeaders()
{
    WRAPPER_NO_CONTRACT;
    return GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->HasNTHeaders();
}

inline BOOL PEImage::HasCorHeader()
{
    WRAPPER_NO_CONTRACT;
    return GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->HasCorHeader();
}

inline BOOL PEImage::IsComponentAssembly()
{
    WRAPPER_NO_CONTRACT;
    return GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->IsComponentAssembly();
}

inline BOOL PEImage::HasReadyToRunHeader()
{
    WRAPPER_NO_CONTRACT;
    return GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->HasReadyToRunHeader();
}

inline BOOL PEImage::HasDirectoryEntry(int entry)
{
    WRAPPER_NO_CONTRACT;
    return GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->HasDirectoryEntry(entry);
}

inline mdToken PEImage::GetEntryPointToken()
{
    WRAPPER_NO_CONTRACT;
    PEImageLayout* pLayout = GetOrCreateLayout(PEImageLayout::LAYOUT_ANY);
    if (!pLayout->HasManagedEntryPoint())
        return mdTokenNil;
    return pLayout->GetEntryPointToken();
}

inline DWORD PEImage::GetCorHeaderFlags()
{
    WRAPPER_NO_CONTRACT;
    return VAL32(GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->GetCorHeader()->Flags);
}

inline BOOL PEImage::MDImportLoaded()
{
    return m_pMDImport != NULL;
}

inline BOOL PEImage::IsILOnly()
{
    WRAPPER_NO_CONTRACT;
    return GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->IsILOnly();
}

inline PTR_CVOID PEImage::GetNativeManifestMetadata(COUNT_T *pSize)
{
    WRAPPER_NO_CONTRACT;
    return GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->GetNativeManifestMetadata(pSize);
}

inline PTR_CVOID PEImage::GetMetadata(COUNT_T *pSize)
{
    WRAPPER_NO_CONTRACT;
    return GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->GetMetadata(pSize);
}

inline BOOL PEImage::HasContents()
{
    WRAPPER_NO_CONTRACT;
    return GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->HasContents();
}


inline CHECK PEImage::CheckFormat()
{
    WRAPPER_NO_CONTRACT;
    CHECK(GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->CheckFormat());
    CHECK_OK;
}

inline void  PEImage::Init(ProbeExtensionResult probeExtensionResult)
{
    CONTRACTL
    {
        THROWS;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;

    m_pathHash = m_path.HashCaseInsensitive();
    m_probeExtensionResult = probeExtensionResult;
    SetModuleFileNameHintForDAC();
}
#ifndef DACCESS_COMPILE


/*static*/
inline PTR_PEImage PEImage::FindByPath(LPCWSTR pPath, BOOL isInBundle, BOOL isExternalData)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION(CheckPointer(pPath));
        PRECONDITION(!(isInBundle && isExternalData));
        PRECONDITION(s_hashLock.OwnedByCurrentThread());
    }
    CONTRACTL_END;

    int CaseHashHelper(const WCHAR *buffer, COUNT_T count);

    PEImageLocator locator(pPath, isInBundle, isExternalData);
    DWORD dwHash = CaseHashHelper(pPath, (COUNT_T) u16_strlen(pPath));
    return (PEImage *) s_Images->LookupValue(dwHash, &locator);
}

/* static */
inline PTR_PEImage PEImage::OpenImage(LPCWSTR pPath, MDInternalImportFlags flags /* = MDInternalImport_Default */, ProbeExtensionResult probeExtensionResult /* = ProbeExtensionResult::Invalid() */)
{
    BOOL forbidCache = (flags & MDInternalImport_NoCache);
    if (forbidCache)
    {
        PEImageHolder pImage(new PEImage{pPath});
        pImage->Init(probeExtensionResult);
        return dac_cast<PTR_PEImage>(pImage.Extract());
    }

    CrstHolder holder(&s_hashLock);

    PEImage* found = FindByPath(pPath, probeExtensionResult.Type == ProbeExtensionResult::Type::Bundle, probeExtensionResult.Type == ProbeExtensionResult::Type::External);
    if (found == (PEImage*) INVALIDENTRY)
    {
        // We did not find the entry in the Cache, and we've been asked to only use the cache.
        if  ((flags & MDInternalImport_OnlyLookInCache) == MDInternalImport_OnlyLookInCache)
        {
            return NULL;
        }

        PEImageHolder pImage(new PEImage{pPath});
        pImage->Init(probeExtensionResult);

        pImage->AddToHashMap();
        return dac_cast<PTR_PEImage>(pImage.Extract());
    }

    found->AddRef();

    return dac_cast<PTR_PEImage>(found);
}
#endif

#ifndef DACCESS_COMPILE

inline void PEImage::AddToHashMap()
{
    CONTRACTL
    {
        THROWS;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;

    _ASSERTE(s_hashLock.OwnedByCurrentThread());
    s_Images->InsertValue(m_pathHash,this);
    m_bInHashMap=TRUE;
}

#endif

inline BOOL PEImage::Has32BitNTHeaders()
{
    WRAPPER_NO_CONTRACT;
    return GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->Has32BitNTHeaders();
}

inline void  PEImage::GetPEKindAndMachine(DWORD* pdwKind, DWORD* pdwMachine)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    // first check if we have a valid PE kind
    if (VolatileLoad(&m_dwPEKind) == 0)
    {
        // Compute result into a local variables first
        DWORD dwPEKind, dwMachine;
        GetOrCreateLayout(PEImageLayout::LAYOUT_ANY)->GetPEKindAndMachine(&dwPEKind, &dwMachine);

        // Write the final results - first machine, then kind.
        m_dwMachine = dwMachine;
        VolatileStore(&m_dwPEKind, dwPEKind);
    }

    *pdwKind = m_dwPEKind;
    *pdwMachine = m_dwMachine;
}

#endif  // PEIMAGE_INL_
