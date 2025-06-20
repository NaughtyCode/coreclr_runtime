// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

//


#include "stdafx.h"

#include "corerror.h"
#include "stgpool.h"


//*****************************************************************************
// Creation for new CCeeGen instances
//
// Both allocate and call virtual Init() (Can't call v-func in a ctor,
// but we want to create in 1 call);
//*****************************************************************************

HRESULT STDMETHODCALLTYPE CreateICeeGen(REFIID riid, void **pCeeGen)
{
    if (riid != IID_ICeeGenInternal)
        return E_NOTIMPL;
    if (!pCeeGen)
        return E_POINTER;
    CCeeGen *pCeeFileGen;
    HRESULT hr = CCeeGen::CreateNewInstance(pCeeFileGen);
    if (FAILED(hr))
        return hr;
    pCeeFileGen->AddRef();
    *(CCeeGen**)pCeeGen = pCeeFileGen;
    return S_OK;
}

HRESULT CCeeGen::CreateNewInstance(CCeeGen* & pGen) // static, public
{
    NewHolder<CCeeGen> pGenHolder(new CCeeGen());
    _ASSERTE(pGenHolder != NULL);
    TESTANDRETURNMEMORY(pGenHolder);

    pGenHolder->m_peSectionMan = new PESectionMan;
    _ASSERTE(pGenHolder->m_peSectionMan != NULL);
    TESTANDRETURNMEMORY(pGenHolder->m_peSectionMan);

    HRESULT hr = pGenHolder->m_peSectionMan->Init();
    if (FAILED(hr))
    {
        pGenHolder->Cleanup();
        return hr;
    }

    hr = pGenHolder->Init();
    if (FAILED(hr))
    {
        // Init() calls Cleanup() on failure
        return hr;
    }

    pGen = pGenHolder.Extract();
    return hr;
}

STDMETHODIMP CCeeGen::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (riid == IID_IUnknown)
        *ppv = (IUnknown*)(ICeeGenInternal*)this;
    else if (riid == IID_ICeeGenInternal)
        *ppv = (ICeeGenInternal*)this;
    if (*ppv == NULL)
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CCeeGen::AddRef(void)
{
    return InterlockedIncrement(&m_cRefs);
}

STDMETHODIMP_(ULONG) CCeeGen::Release(void)
{
    if (InterlockedDecrement(&m_cRefs) == 0) {
        Cleanup();
        delete this;
        return 0;
    }
    return 1;
}

STDMETHODIMP CCeeGen::SetInitialGrowth(DWORD growth)
{
    getIlSection().SetInitialGrowth(growth);

    return S_OK;
}

STDMETHODIMP CCeeGen::EmitString (_In_ LPWSTR lpString, ULONG *RVA)
{
    HRESULT hr = S_OK;

    if (! RVA)
        IfFailGo(E_POINTER);
    hr = getStringSection().getEmittedStringRef(lpString, RVA);
ErrExit:
    return hr;
}

STDMETHODIMP CCeeGen::AllocateMethodBuffer(ULONG cchBuffer, UCHAR **lpBuffer, ULONG *RVA)
{
    _ASSERTE(lpBuffer != NULL);
    _ASSERTE(RVA != NULL);

    if (cchBuffer == 0)
        return E_INVALIDARG;

    uint32_t const Alignment = sizeof(DWORD); // DWORD align
    ULONG cchBufferRequest = cchBuffer;

    // Method offsets of 0 are special in ECMA-335 (See I.9.4 and II.22.26).
    // In order to be consistent with those sections we avoid returning an RVA
    // of 0. If the IL section is empty we will add a minimal amount of padding
    // to avoid RVAs of 0.
    if (getIlSection().dataLen() == 0)
        cchBufferRequest += Alignment;

    *lpBuffer = (UCHAR*)getIlSection().getBlock(cchBufferRequest, Alignment);
    if (*lpBuffer == NULL)
        return E_OUTOFMEMORY;

    // Compute the method offset after getting the block, not
    // before (since alignment might shift it up
    // for in-memory, just return address and will calc later.
    ULONG methodOffset = getIlSection().dataLen() - cchBuffer;
    _ASSERTE(methodOffset != 0);

    *RVA = methodOffset;
    return S_OK;
}

STDMETHODIMP CCeeGen::GetMethodBuffer(ULONG RVA, UCHAR **lpBuffer)
{
    HRESULT hr = E_FAIL;
    _ASSERTE(RVA != 0);

    if (! lpBuffer)
        IfFailGo(E_POINTER);
    *lpBuffer = (UCHAR*)getIlSection().computePointer(RVA);

ErrExit:
    if (lpBuffer != NULL && *lpBuffer != 0)
        return S_OK;

    return hr;
}

STDMETHODIMP CCeeGen::ComputePointer(HCEESECTION section, ULONG RVA, UCHAR **lpBuffer)
{
    HRESULT hr = E_FAIL;

    if (! lpBuffer)
        IfFailGo(E_POINTER);
    *lpBuffer = (UCHAR*) ((CeeSection *)section)->computePointer(RVA);

ErrExit:
    if (lpBuffer != NULL && *lpBuffer != 0)
        return S_OK;
    return hr;
}

STDMETHODIMP CCeeGen::GenerateCeeFile ()
{
    _ASSERTE(!"E_NOTIMPL");
    return E_NOTIMPL;
}

STDMETHODIMP CCeeGen::GetIlSection (
        HCEESECTION *section)
{
    *section = (HCEESECTION)(m_sections[m_ilIdx]);
    return S_OK;
}

STDMETHODIMP CCeeGen::GetStringSection(HCEESECTION *section)
{
    _ASSERTE(!"E_NOTIMPL");
    return E_NOTIMPL;
}

STDMETHODIMP CCeeGen::AddSectionReloc (
        HCEESECTION section,
        ULONG offset,
        HCEESECTION relativeTo,
        CeeSectionRelocType relocType)
{
    return m_sections[m_ilIdx]->addSectReloc(offset, *(m_sections[m_ilIdx]), relocType);
}

STDMETHODIMP CCeeGen::GetSectionCreate (
        const char *name,
        DWORD flags,
        HCEESECTION *section)
{
    short       sectionIdx;
    return getSectionCreate (name, flags, (CeeSection **)section, &sectionIdx);
}

STDMETHODIMP CCeeGen::GetSectionDataLen (
        HCEESECTION section,
        ULONG *dataLen)
{
    CeeSection *pSection = (CeeSection*) section;
    *dataLen = pSection->dataLen();

    return NOERROR;
}

STDMETHODIMP CCeeGen::GetSectionBlock (
        HCEESECTION section,
        ULONG len,
        ULONG align,
        void **ppBytes)
{
    CeeSection *pSection = (CeeSection*) section;
    *ppBytes = (BYTE *)pSection->getBlock(len, align);

    if (*ppBytes == 0)
        return E_OUTOFMEMORY;
    return NOERROR;
}


CCeeGen::CCeeGen() // protected ctor
{
// All other init done in InitCommon()
    m_cRefs = 0;
    m_peSectionMan = NULL;
    m_pTokenMap = NULL;
}

// Shared init code between derived classes, called by virtual Init()
HRESULT CCeeGen::Init() // not-virtual, protected
{
// Public, Virtual init must create our SectionManager, and
// Common init does the rest
    _ASSERTE(m_peSectionMan != NULL);

    HRESULT hr = S_OK;

    PESection *section = NULL;
    CeeSection *ceeSection = NULL;

    m_corHeader = NULL;

    m_numSections = 0;
    m_allocSections = 10;
    m_sections = new CeeSection * [ m_allocSections ];
    if (m_sections == NULL) {
        hr = E_OUTOFMEMORY;
        goto LExit;
    }

    m_pTokenMap = NULL;
    m_fTokenMapSupported = FALSE;

    // These text section needs special support for handling string management now that we have
    // merged the sections together, so create it with an underlying CeeSectionString rather than the
    // more generic CeeSection

    hr = m_peSectionMan->getSectionCreate(".text", sdExecute, &section);
    if (FAILED(hr)) {
        goto LExit;
    }

    ceeSection = new CeeSectionString(*this, *section);
    if (ceeSection == NULL) {
        hr = E_OUTOFMEMORY;
        goto LExit;
    }

    hr = addSection(ceeSection, &m_stringIdx);

    m_textIdx = m_stringIdx;

    m_metaIdx = m_textIdx;  // meta section is actually in .text
    m_ilIdx = m_textIdx;    // il section is actually in .text
    m_corHdrIdx = -1;

LExit:
    if (FAILED(hr)) {
        Cleanup();
    }

    return hr;
}

HRESULT CCeeGen::Cleanup() // virtual
{
    HRESULT hr;
    for (int i = 0; i < m_numSections; i++) {
        delete m_sections[i];
    }

    delete [] m_sections;

    CeeGenTokenMapper *pMapper = m_pTokenMap;
    if (pMapper) {
        if (pMapper->m_pIImport) {
            IMetaDataEmit *pIIEmit;
            if (SUCCEEDED( hr = pMapper->m_pIImport->QueryInterface(IID_IMetaDataEmit, (void **) &pIIEmit)))
            {
                pIIEmit->SetHandler(NULL);
                pIIEmit->Release();
            }
            _ASSERTE(SUCCEEDED(hr));
            pMapper->m_pIImport->Release();
        }
        pMapper->Release();
        m_pTokenMap = NULL;
    }

    if (m_peSectionMan) {
        m_peSectionMan->Cleanup();
        delete m_peSectionMan;
    }

    return S_OK;
}

HRESULT CCeeGen::addSection(CeeSection *section, short *sectionIdx)
{
    if (m_numSections >= m_allocSections)
    {
        _ASSERTE(m_allocSections > 0);
        while (m_numSections >= m_allocSections)
            m_allocSections <<= 1;
        CeeSection **newSections = new CeeSection * [m_allocSections];
        if (newSections == NULL)
            return E_OUTOFMEMORY;
        CopyMemory(newSections, m_sections, m_numSections * sizeof(*m_sections));
        if (m_sections != NULL)
            delete [] m_sections;
        m_sections = newSections;
    }

    if (sectionIdx)
        *sectionIdx = m_numSections;

    m_sections[m_numSections++] = section;
    return S_OK;
}

HRESULT CCeeGen::getSectionCreate (const char *name, DWORD flags, CeeSection **section, short *sectionIdx)
{
    if (strcmp(name, ".il") == 0)
        name = ".text";
    else if (strcmp(name, ".meta") == 0)
        name = ".text";
    else if (strcmp(name, ".rdata") == 0)
        name = ".text";
    for (short i=0; i<m_numSections; i++) {
        if (strcmp((const char *)m_sections[i]->name(), name) == 0) {
            if (section)
                *section = m_sections[i];
            if (sectionIdx)
                *sectionIdx = i;
            return S_OK;
        }
    }
    PESection *pewSect = NULL;
    HRESULT hr = m_peSectionMan->getSectionCreate(name, flags, &pewSect);
    TESTANDRETURNHR(hr);
    CeeSection *newSect = new CeeSection(*this, *pewSect);
    // if this fails, the PESection will get zapped in the destructor for CCeeGen
    if (newSect == NULL)
    {
        return E_OUTOFMEMORY;
    }

    hr = addSection(newSect, sectionIdx);
    TESTANDRETURNHR(hr);
    if (section)
        *section = newSect;
    return S_OK;
}


HRESULT CCeeGen::emitMetaData(IMetaDataEmit *emitter, CeeSection* section, DWORD offset, BYTE* buffer, unsigned buffLen)
{
    HRESULT hr = S_OK;

    ReleaseHolder<IStream> metaStream(new(std::nothrow) CGrowableStream());

    if (metaStream == NULL)
        return E_OUTOFMEMORY;

    if (! m_fTokenMapSupported) {
        IUnknown *pMapTokenIface;
        IfFailGoto(getMapTokenIface(&pMapTokenIface, emitter), Exit);

        // Set a callback for token remap and save the tokens which change.
        IfFailGoto(emitter->SetHandler(pMapTokenIface), Exit);
    }

    // generate the metadata
    IfFailGoto(emitter->SaveToStream(metaStream, 0), Exit);

    // get size of stream and get sufficient storage for it

    if (section == 0) {
        section = &getMetaSection();
        STATSTG statStg;
        IfFailGoto((HRESULT)(metaStream->Stat(&statStg, STATFLAG_NONAME)), Exit);

        buffLen = statStg.cbSize.u.LowPart;
        buffer = (BYTE *)section->getBlock(buffLen, sizeof(DWORD));
        IfNullGoto(buffer, Exit);
        offset = getMetaSection().dataLen() - buffLen;
    }
    else {
        _ASSERTE(buffer[buffLen-1] || true); // Dereference 'buffer'
        _ASSERTE(section->computeOffset(PCHAR(buffer)) == offset);
    }

    // reset seek pointer and read from stream
    {
        LARGE_INTEGER disp = { {0, 0} };
        IfFailGoto((HRESULT)metaStream->Seek(disp, STREAM_SEEK_SET, NULL), Exit);
    }
    ULONG metaDataLen;
    IfFailGoto((HRESULT)metaStream->Read(buffer, buffLen, &metaDataLen), Exit);

    _ASSERTE(metaDataLen <= buffLen);

    // Set meta virtual address to offset of metadata within .meta, and
    // and add a reloc for this offset, which will get turned
    // into an rva when the pewriter writes out the file.

    m_corHeader->MetaData.VirtualAddress = VAL32(offset);
    getCorHeaderSection().addSectReloc(m_corHeaderOffset + offsetof(IMAGE_COR20_HEADER, MetaData), *section, srRelocAbsolute);
    m_corHeader->MetaData.Size = VAL32(metaDataLen);

Exit:

    if (! m_fTokenMapSupported) {
        // Remove the handler that we set
        hr = emitter->SetHandler(NULL);
    }

#ifdef _DEBUG
    if (FAILED(hr) && hr != E_OUTOFMEMORY)
        _ASSERTE(!"Unexpected Failure");
#endif

    return hr;
}

// Create the COM header - it goes at front of .meta section
// Need to do this before the meta data is copied in, but don't do at
// the same time because may not have metadata
HRESULT CCeeGen::allocateCorHeader()
{
    HRESULT hr = S_OK;
    CeeSection *corHeaderSection = NULL;
    if (m_corHdrIdx < 0) {
        hr = getSectionCreate(".text0", sdExecute, &corHeaderSection, &m_corHdrIdx);
        TESTANDRETURNHR(hr);

        m_corHeaderOffset = corHeaderSection->dataLen();
        m_corHeader = (IMAGE_COR20_HEADER*)corHeaderSection->getBlock(sizeof(IMAGE_COR20_HEADER));
        if (! m_corHeader)
            return E_OUTOFMEMORY;
        memset(m_corHeader, 0, sizeof(IMAGE_COR20_HEADER));
    }
    return S_OK;
}

HRESULT CCeeGen::getMethodRVA(ULONG codeOffset, ULONG *codeRVA)
{
    _ASSERTE(codeRVA);
    // for runtime conversion, just return the offset and will calculate real address when need the code
    *codeRVA = codeOffset;
    return S_OK;
}

HRESULT CCeeGen::getMapTokenIface(IUnknown **pIMapToken, IMetaDataEmit *emitter)
{
    if (! pIMapToken)
        return E_POINTER;
    if (! m_pTokenMap) {
        // Allocate the token mapper. As code is generated, each moved token will be added to
        // the mapper and the client will also add a TokenMap reloc for it so we can update later
        CeeGenTokenMapper *pMapper = new CeeGenTokenMapper;
        if (pMapper == NULL)
        {
            return E_OUTOFMEMORY;
        }

        if (emitter) {
            HRESULT hr;
            hr = emitter->QueryInterface(IID_IMetaDataImport, (PVOID *) &pMapper->m_pIImport);
            _ASSERTE(SUCCEEDED(hr));
        }
        m_pTokenMap = pMapper;
        m_fTokenMapSupported = (emitter == 0);
    }
    *pIMapToken = getTokenMapper()->GetMapTokenIface();
    return S_OK;
}
