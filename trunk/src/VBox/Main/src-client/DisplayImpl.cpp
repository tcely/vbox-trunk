/* $Id$ */
/** @file
 * VirtualBox COM class implementation
 */

/*
 * Copyright (C) 2006-2014 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#include "DisplayImpl.h"
#include "DisplayUtils.h"
#include "ConsoleImpl.h"
#include "ConsoleVRDPServer.h"
#include "VMMDev.h"

#include "AutoCaller.h"
#include "Logging.h"

/* generated header */
#include "VBoxEvents.h"

#include <iprt/semaphore.h>
#include <iprt/thread.h>
#include <iprt/asm.h>
#include <iprt/time.h>
#include <iprt/cpp/utils.h>
#include <iprt/alloca.h>

#include <VBox/vmm/pdmdrv.h>
#if defined(DEBUG) || defined(VBOX_STRICT) /* for VM_ASSERT_EMT(). */
# include <VBox/vmm/vm.h>
#endif

#ifdef VBOX_WITH_VIDEOHWACCEL
# include <VBox/VBoxVideo.h>
#endif

#if defined(VBOX_WITH_CROGL) || defined(VBOX_WITH_CRHGSMI)
# include <VBox/HostServices/VBoxCrOpenGLSvc.h>
#endif

#include <VBox/com/array.h>

#ifdef VBOX_WITH_VPX
# include <iprt/path.h>
# include "VideoRec.h"
#endif

#ifdef VBOX_WITH_CROGL
typedef enum
{
    CRVREC_STATE_IDLE,
    CRVREC_STATE_SUBMITTED
} CRVREC_STATE;
#endif

/**
 * Display driver instance data.
 *
 * @implements PDMIDISPLAYCONNECTOR
 */
typedef struct DRVMAINDISPLAY
{
    /** Pointer to the display object. */
    Display                    *pDisplay;
    /** Pointer to the driver instance structure. */
    PPDMDRVINS                  pDrvIns;
    /** Pointer to the keyboard port interface of the driver/device above us. */
    PPDMIDISPLAYPORT            pUpPort;
    /** Our display connector interface. */
    PDMIDISPLAYCONNECTOR        IConnector;
#if defined(VBOX_WITH_VIDEOHWACCEL) || defined(VBOX_WITH_CRHGSMI)
    /** VBVA callbacks */
    PPDMIDISPLAYVBVACALLBACKS   pVBVACallbacks;
#endif
} DRVMAINDISPLAY, *PDRVMAINDISPLAY;

/** Converts PDMIDISPLAYCONNECTOR pointer to a DRVMAINDISPLAY pointer. */
#define PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface)  RT_FROM_MEMBER(pInterface, DRVMAINDISPLAY, IConnector)

// constructor / destructor
/////////////////////////////////////////////////////////////////////////////

Display::Display()
    : mParent(NULL)
{
}

Display::~Display()
{
}


HRESULT Display::FinalConstruct()
{
    int rc = videoAccelConstruct(&mVideoAccelLegacy);
    AssertRC(rc);

    mfVideoAccelVRDP = false;
    mfu32SupportedOrders = 0;
    mcVideoAccelVRDPRefs = 0;

#ifdef VBOX_WITH_CROGL
    mfCrOglDataHidden = false;
#endif

    mpDrv = NULL;
    mpVMMDev = NULL;
    mfVMMDevInited = false;

    rc = RTCritSectInit(&mVideoAccelLock);
    AssertRC(rc);

#ifdef VBOX_WITH_HGSMI
    mu32UpdateVBVAFlags = 0;
#endif
#ifdef VBOX_WITH_VPX
    mpVideoRecCtx = NULL;
    for (unsigned i = 0; i < RT_ELEMENTS(maVideoRecEnabled); i++)
        maVideoRecEnabled[i] = true;
#endif

#ifdef VBOX_WITH_CRHGSMI
    mhCrOglSvc = NULL;
    rc = RTCritSectRwInit(&mCrOglLock);
    AssertRC(rc);
#endif
#ifdef VBOX_WITH_CROGL
    RT_ZERO(mCrOglCallbacks);
    RT_ZERO(mCrOglScreenshotData);
    mfCrOglVideoRecState = CRVREC_STATE_IDLE;
    mCrOglScreenshotData.u32Screen = CRSCREEN_ALL;
    mCrOglScreenshotData.pvContext = this;
    mCrOglScreenshotData.pfnScreenshotBegin = i_displayCrVRecScreenshotBegin;
    mCrOglScreenshotData.pfnScreenshotPerform = i_displayCrVRecScreenshotPerform;
    mCrOglScreenshotData.pfnScreenshotEnd = i_displayCrVRecScreenshotEnd;
#endif

    return BaseFinalConstruct();
}

void Display::FinalRelease()
{
    uninit();

    videoAccelDestroy(&mVideoAccelLegacy);

    if (RTCritSectIsInitialized(&mVideoAccelLock))
    {
        RTCritSectDelete(&mVideoAccelLock);
        RT_ZERO(mVideoAccelLock);
    }

#ifdef VBOX_WITH_CRHGSMI
    if (RTCritSectRwIsInitialized (&mCrOglLock))
    {
        RTCritSectRwDelete (&mCrOglLock);
        RT_ZERO(mCrOglLock);
    }
#endif
    BaseFinalRelease();
}

// public initializer/uninitializer for internal purposes only
/////////////////////////////////////////////////////////////////////////////

#define kMaxSizeThumbnail 64

/**
 * Save thumbnail and screenshot of the guest screen.
 */
static int displayMakeThumbnail(uint8_t *pu8Data, uint32_t cx, uint32_t cy,
                                uint8_t **ppu8Thumbnail, uint32_t *pcbThumbnail, uint32_t *pcxThumbnail, uint32_t *pcyThumbnail)
{
    int rc = VINF_SUCCESS;

    uint8_t *pu8Thumbnail = NULL;
    uint32_t cbThumbnail = 0;
    uint32_t cxThumbnail = 0;
    uint32_t cyThumbnail = 0;

    if (cx > cy)
    {
        cxThumbnail = kMaxSizeThumbnail;
        cyThumbnail = (kMaxSizeThumbnail * cy) / cx;
    }
    else
    {
        cyThumbnail = kMaxSizeThumbnail;
        cxThumbnail = (kMaxSizeThumbnail * cx) / cy;
    }

    LogRelFlowFunc(("%dx%d -> %dx%d\n", cx, cy, cxThumbnail, cyThumbnail));

    cbThumbnail = cxThumbnail * 4 * cyThumbnail;
    pu8Thumbnail = (uint8_t *)RTMemAlloc(cbThumbnail);

    if (pu8Thumbnail)
    {
        uint8_t *dst = pu8Thumbnail;
        uint8_t *src = pu8Data;
        int dstW = cxThumbnail;
        int dstH = cyThumbnail;
        int srcW = cx;
        int srcH = cy;
        int iDeltaLine = cx * 4;

        BitmapScale32(dst,
                      dstW, dstH,
                      src,
                      iDeltaLine,
                      srcW, srcH);

        *ppu8Thumbnail = pu8Thumbnail;
        *pcbThumbnail = cbThumbnail;
        *pcxThumbnail = cxThumbnail;
        *pcyThumbnail = cyThumbnail;
    }
    else
    {
        rc = VERR_NO_MEMORY;
    }

    return rc;
}

#ifdef VBOX_WITH_CROGL
typedef struct
{
    CRVBOXHGCMTAKESCREENSHOT Base;

    /* 32bpp small RGB image. */
    uint8_t *pu8Thumbnail;
    uint32_t cbThumbnail;
    uint32_t cxThumbnail;
    uint32_t cyThumbnail;

    /* PNG screenshot. */
    uint8_t *pu8PNG;
    uint32_t cbPNG;
    uint32_t cxPNG;
    uint32_t cyPNG;
} VBOX_DISPLAY_SAVESCREENSHOT_DATA;

static DECLCALLBACK(void) displaySaveScreenshotReport(void *pvCtx, uint32_t uScreen,
        uint32_t x, uint32_t y, uint32_t uBitsPerPixel,
        uint32_t uBytesPerLine, uint32_t uGuestWidth, uint32_t uGuestHeight,
        uint8_t *pu8BufferAddress, uint64_t u64TimeStamp)
{
    VBOX_DISPLAY_SAVESCREENSHOT_DATA *pData = (VBOX_DISPLAY_SAVESCREENSHOT_DATA*)pvCtx;
    displayMakeThumbnail(pu8BufferAddress, uGuestWidth, uGuestHeight, &pData->pu8Thumbnail,
                         &pData->cbThumbnail, &pData->cxThumbnail, &pData->cyThumbnail);
    int rc = DisplayMakePNG(pu8BufferAddress, uGuestWidth, uGuestHeight, &pData->pu8PNG,
                            &pData->cbPNG, &pData->cxPNG, &pData->cyPNG, 1);
    if (RT_FAILURE(rc))
    {
        AssertMsgFailed(("DisplayMakePNG failed %d\n", rc));
        if (pData->pu8PNG)
        {
            RTMemFree(pData->pu8PNG);
            pData->pu8PNG = NULL;
        }
        pData->cbPNG = 0;
        pData->cxPNG = 0;
        pData->cyPNG = 0;
    }
}
#endif

DECLCALLBACK(void) Display::i_displaySSMSaveScreenshot(PSSMHANDLE pSSM, void *pvUser)
{
    Display *that = static_cast<Display*>(pvUser);

    /* 32bpp small RGB image. */
    uint8_t *pu8Thumbnail = NULL;
    uint32_t cbThumbnail = 0;
    uint32_t cxThumbnail = 0;
    uint32_t cyThumbnail = 0;

    /* PNG screenshot. */
    uint8_t *pu8PNG = NULL;
    uint32_t cbPNG = 0;
    uint32_t cxPNG = 0;
    uint32_t cyPNG = 0;

    Console::SafeVMPtr ptrVM(that->mParent);
    if (ptrVM.isOk())
    {
        /* Query RGB bitmap. */
        uint8_t *pu8Data = NULL;
        size_t cbData = 0;
        uint32_t cx = 0;
        uint32_t cy = 0;

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
        BOOL f3DSnapshot = FALSE;
        BOOL is3denabled;
        that->mParent->i_machine()->COMGETTER(Accelerate3DEnabled)(&is3denabled);
        if (is3denabled && that->mCrOglCallbacks.pfnHasData())
        {
            VMMDev *pVMMDev = that->mParent->i_getVMMDev();
            if (pVMMDev)
            {
                VBOX_DISPLAY_SAVESCREENSHOT_DATA *pScreenshot =
                    (VBOX_DISPLAY_SAVESCREENSHOT_DATA*)RTMemAllocZ(sizeof(*pScreenshot));
                if (pScreenshot)
                {
                    /* screen id or CRSCREEN_ALL to specify all enabled */
                    pScreenshot->Base.u32Screen = 0;
                    pScreenshot->Base.u32Width = 0;
                    pScreenshot->Base.u32Height = 0;
                    pScreenshot->Base.u32Pitch = 0;
                    pScreenshot->Base.pvBuffer = NULL;
                    pScreenshot->Base.pvContext = pScreenshot;
                    pScreenshot->Base.pfnScreenshotBegin = NULL;
                    pScreenshot->Base.pfnScreenshotPerform = displaySaveScreenshotReport;
                    pScreenshot->Base.pfnScreenshotEnd = NULL;

                    VBOXCRCMDCTL_HGCM data;
                    data.Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
                    data.Hdr.u32Function = SHCRGL_HOST_FN_TAKE_SCREENSHOT;

                    data.aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
                    data.aParms[0].u.pointer.addr = &pScreenshot->Base;
                    data.aParms[0].u.pointer.size = sizeof(pScreenshot->Base);

                    int rc = that->i_crCtlSubmitSync(&data.Hdr, sizeof(data));
                    if (RT_SUCCESS(rc))
                    {
                        if (pScreenshot->pu8PNG)
                        {
                            pu8Thumbnail = pScreenshot->pu8Thumbnail;
                            cbThumbnail = pScreenshot->cbThumbnail;
                            cxThumbnail = pScreenshot->cxThumbnail;
                            cyThumbnail = pScreenshot->cyThumbnail;

                            /* PNG screenshot. */
                            pu8PNG = pScreenshot->pu8PNG;
                            cbPNG = pScreenshot->cbPNG;
                            cxPNG = pScreenshot->cxPNG;
                            cyPNG = pScreenshot->cyPNG;
                            f3DSnapshot = TRUE;
                        }
                        else
                            AssertMsgFailed(("no png\n"));
                    }
                    else
                        AssertMsgFailed(("SHCRGL_HOST_FN_TAKE_SCREENSHOT failed %d\n", rc));


                    RTMemFree(pScreenshot);
                }
            }
        }

        if (!f3DSnapshot)
#endif
        {
            /* SSM code is executed on EMT(0), therefore no need to use VMR3ReqCallWait. */
            int rc = Display::i_displayTakeScreenshotEMT(that, VBOX_VIDEO_PRIMARY_SCREEN, &pu8Data, &cbData, &cx, &cy);

            /*
             * It is possible that success is returned but everything is 0 or NULL.
             * (no display attached if a VM is running with VBoxHeadless on OSE for example)
             */
            if (RT_SUCCESS(rc) && pu8Data)
            {
                Assert(cx && cy);

                /* Prepare a small thumbnail and a PNG screenshot. */
                displayMakeThumbnail(pu8Data, cx, cy, &pu8Thumbnail, &cbThumbnail, &cxThumbnail, &cyThumbnail);
                rc = DisplayMakePNG(pu8Data, cx, cy, &pu8PNG, &cbPNG, &cxPNG, &cyPNG, 1);
                if (RT_FAILURE(rc))
                {
                    if (pu8PNG)
                    {
                        RTMemFree(pu8PNG);
                        pu8PNG = NULL;
                    }
                    cbPNG = 0;
                    cxPNG = 0;
                    cyPNG = 0;
                }

                /* This can be called from any thread. */
                that->mpDrv->pUpPort->pfnFreeScreenshot(that->mpDrv->pUpPort, pu8Data);
            }
        }
    }
    else
    {
        LogFunc(("Failed to get VM pointer 0x%x\n", ptrVM.rc()));
    }

    /* Regardless of rc, save what is available:
     * Data format:
     *    uint32_t cBlocks;
     *    [blocks]
     *
     *  Each block is:
     *    uint32_t cbBlock;        if 0 - no 'block data'.
     *    uint32_t typeOfBlock;    0 - 32bpp RGB bitmap, 1 - PNG, ignored if 'cbBlock' is 0.
     *    [block data]
     *
     *  Block data for bitmap and PNG:
     *    uint32_t cx;
     *    uint32_t cy;
     *    [image data]
     */
    SSMR3PutU32(pSSM, 2); /* Write thumbnail and PNG screenshot. */

    /* First block. */
    SSMR3PutU32(pSSM, cbThumbnail + 2 * sizeof(uint32_t));
    SSMR3PutU32(pSSM, 0); /* Block type: thumbnail. */

    if (cbThumbnail)
    {
        SSMR3PutU32(pSSM, cxThumbnail);
        SSMR3PutU32(pSSM, cyThumbnail);
        SSMR3PutMem(pSSM, pu8Thumbnail, cbThumbnail);
    }

    /* Second block. */
    SSMR3PutU32(pSSM, cbPNG + 2 * sizeof(uint32_t));
    SSMR3PutU32(pSSM, 1); /* Block type: png. */

    if (cbPNG)
    {
        SSMR3PutU32(pSSM, cxPNG);
        SSMR3PutU32(pSSM, cyPNG);
        SSMR3PutMem(pSSM, pu8PNG, cbPNG);
    }

    RTMemFree(pu8PNG);
    RTMemFree(pu8Thumbnail);
}

DECLCALLBACK(int)
Display::i_displaySSMLoadScreenshot(PSSMHANDLE pSSM, void *pvUser, uint32_t uVersion, uint32_t uPass)
{
    Display *that = static_cast<Display*>(pvUser);

    if (uVersion != sSSMDisplayScreenshotVer)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;
    Assert(uPass == SSM_PASS_FINAL); NOREF(uPass);

    /* Skip data. */
    uint32_t cBlocks;
    int rc = SSMR3GetU32(pSSM, &cBlocks);
    AssertRCReturn(rc, rc);

    for (uint32_t i = 0; i < cBlocks; i++)
    {
        uint32_t cbBlock;
        rc = SSMR3GetU32(pSSM, &cbBlock);
        AssertRCBreak(rc);

        uint32_t typeOfBlock;
        rc = SSMR3GetU32(pSSM, &typeOfBlock);
        AssertRCBreak(rc);

        LogRelFlowFunc(("[%d] type %d, size %d bytes\n", i, typeOfBlock, cbBlock));

        /* Note: displaySSMSaveScreenshot writes size of a block = 8 and
         * do not write any data if the image size was 0.
         * @todo Fix and increase saved state version.
         */
        if (cbBlock > 2 * sizeof(uint32_t))
        {
            rc = SSMR3Skip(pSSM, cbBlock);
            AssertRCBreak(rc);
        }
    }

    return rc;
}

/**
 * Save/Load some important guest state
 */
DECLCALLBACK(void)
Display::i_displaySSMSave(PSSMHANDLE pSSM, void *pvUser)
{
    Display *that = static_cast<Display*>(pvUser);

    SSMR3PutU32(pSSM, that->mcMonitors);
    for (unsigned i = 0; i < that->mcMonitors; i++)
    {
        SSMR3PutU32(pSSM, that->maFramebuffers[i].u32Offset);
        SSMR3PutU32(pSSM, that->maFramebuffers[i].u32MaxFramebufferSize);
        SSMR3PutU32(pSSM, that->maFramebuffers[i].u32InformationSize);
        SSMR3PutU32(pSSM, that->maFramebuffers[i].w);
        SSMR3PutU32(pSSM, that->maFramebuffers[i].h);
        SSMR3PutS32(pSSM, that->maFramebuffers[i].xOrigin);
        SSMR3PutS32(pSSM, that->maFramebuffers[i].yOrigin);
        SSMR3PutU32(pSSM, that->maFramebuffers[i].flags);
    }
}

DECLCALLBACK(int)
Display::i_displaySSMLoad(PSSMHANDLE pSSM, void *pvUser, uint32_t uVersion, uint32_t uPass)
{
    Display *that = static_cast<Display*>(pvUser);

    if (!(   uVersion == sSSMDisplayVer
          || uVersion == sSSMDisplayVer2
          || uVersion == sSSMDisplayVer3))
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;
    Assert(uPass == SSM_PASS_FINAL); NOREF(uPass);

    uint32_t cMonitors;
    int rc = SSMR3GetU32(pSSM, &cMonitors);
    if (cMonitors != that->mcMonitors)
        return SSMR3SetCfgError(pSSM, RT_SRC_POS, N_("Number of monitors changed (%d->%d)!"), cMonitors, that->mcMonitors);

    for (uint32_t i = 0; i < cMonitors; i++)
    {
        SSMR3GetU32(pSSM, &that->maFramebuffers[i].u32Offset);
        SSMR3GetU32(pSSM, &that->maFramebuffers[i].u32MaxFramebufferSize);
        SSMR3GetU32(pSSM, &that->maFramebuffers[i].u32InformationSize);
        if (   uVersion == sSSMDisplayVer2
            || uVersion == sSSMDisplayVer3)
        {
            uint32_t w;
            uint32_t h;
            SSMR3GetU32(pSSM, &w);
            SSMR3GetU32(pSSM, &h);
            that->maFramebuffers[i].w = w;
            that->maFramebuffers[i].h = h;
        }
        if (uVersion == sSSMDisplayVer3)
        {
            int32_t xOrigin;
            int32_t yOrigin;
            uint32_t flags;
            SSMR3GetS32(pSSM, &xOrigin);
            SSMR3GetS32(pSSM, &yOrigin);
            SSMR3GetU32(pSSM, &flags);
            that->maFramebuffers[i].xOrigin = xOrigin;
            that->maFramebuffers[i].yOrigin = yOrigin;
            that->maFramebuffers[i].flags = (uint16_t)flags;
            that->maFramebuffers[i].fDisabled = (that->maFramebuffers[i].flags & VBVA_SCREEN_F_DISABLED) != 0;
        }
    }

    return VINF_SUCCESS;
}

/**
 * Initializes the display object.
 *
 * @returns COM result indicator
 * @param parent          handle of our parent object
 * @param qemuConsoleData address of common console data structure
 */
HRESULT Display::init(Console *aParent)
{
    ComAssertRet(aParent, E_INVALIDARG);
    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    unconst(mParent) = aParent;

    mfSourceBitmapEnabled = true;
    fVGAResizing = false;

    ULONG ul;
    mParent->i_machine()->COMGETTER(MonitorCount)(&ul);
    mcMonitors = ul;

    for (ul = 0; ul < mcMonitors; ul++)
    {
        maFramebuffers[ul].u32Offset = 0;
        maFramebuffers[ul].u32MaxFramebufferSize = 0;
        maFramebuffers[ul].u32InformationSize = 0;

        maFramebuffers[ul].pFramebuffer = NULL;
        /* All secondary monitors are disabled at startup. */
        maFramebuffers[ul].fDisabled = ul > 0;

        maFramebuffers[ul].u32Caps = 0;

        maFramebuffers[ul].updateImage.pu8Address = NULL;
        maFramebuffers[ul].updateImage.cbLine = 0;

        maFramebuffers[ul].xOrigin = 0;
        maFramebuffers[ul].yOrigin = 0;

        maFramebuffers[ul].w = 0;
        maFramebuffers[ul].h = 0;

        maFramebuffers[ul].flags = maFramebuffers[ul].fDisabled? VBVA_SCREEN_F_DISABLED: 0;

        maFramebuffers[ul].u16BitsPerPixel = 0;
        maFramebuffers[ul].pu8FramebufferVRAM = NULL;
        maFramebuffers[ul].u32LineSize = 0;

        maFramebuffers[ul].pHostEvents = NULL;

        maFramebuffers[ul].fDefaultFormat = false;

#ifdef VBOX_WITH_HGSMI
        maFramebuffers[ul].fVBVAEnabled = false;
        maFramebuffers[ul].fVBVAForceResize = false;
        maFramebuffers[ul].fRenderThreadMode = false;
        maFramebuffers[ul].pVBVAHostFlags = NULL;
#endif /* VBOX_WITH_HGSMI */
#ifdef VBOX_WITH_CROGL
        RT_ZERO(maFramebuffers[ul].pendingViewportInfo);
#endif
    }

    {
        // register listener for state change events
        ComPtr<IEventSource> es;
        mParent->COMGETTER(EventSource)(es.asOutParam());
        com::SafeArray<VBoxEventType_T> eventTypes;
        eventTypes.push_back(VBoxEventType_OnStateChanged);
        es->RegisterListener(this, ComSafeArrayAsInParam(eventTypes), true);
    }

    /* Confirm a successful initialization */
    autoInitSpan.setSucceeded();

    return S_OK;
}

/**
 *  Uninitializes the instance and sets the ready flag to FALSE.
 *  Called either from FinalRelease() or by the parent when it gets destroyed.
 */
void Display::uninit()
{
    LogRelFlowFunc(("this=%p\n", this));

    /* Enclose the state transition Ready->InUninit->NotReady */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

    unsigned uScreenId;
    for (uScreenId = 0; uScreenId < mcMonitors; uScreenId++)
    {
        maFramebuffers[uScreenId].pSourceBitmap.setNull();
        maFramebuffers[uScreenId].updateImage.pSourceBitmap.setNull();
        maFramebuffers[uScreenId].updateImage.pu8Address = NULL;
        maFramebuffers[uScreenId].updateImage.cbLine = 0;
        maFramebuffers[uScreenId].pFramebuffer.setNull();
    }

    if (mParent)
    {
        ComPtr<IEventSource> es;
        mParent->COMGETTER(EventSource)(es.asOutParam());
        es->UnregisterListener(this);
    }

    unconst(mParent) = NULL;

    if (mpDrv)
        mpDrv->pDisplay = NULL;

    mpDrv = NULL;
    mpVMMDev = NULL;
    mfVMMDevInited = true;
}

/**
 * Register the SSM methods. Called by the power up thread to be able to
 * pass pVM
 */
int Display::i_registerSSM(PUVM pUVM)
{
    /* Version 2 adds width and height of the framebuffer; version 3 adds
     * the framebuffer offset in the virtual desktop and the framebuffer flags.
     */
    int rc = SSMR3RegisterExternal(pUVM, "DisplayData", 0, sSSMDisplayVer3,
                                   mcMonitors * sizeof(uint32_t) * 8 + sizeof(uint32_t),
                                   NULL, NULL, NULL,
                                   NULL, i_displaySSMSave, NULL,
                                   NULL, i_displaySSMLoad, NULL, this);
    AssertRCReturn(rc, rc);

    /*
     * Register loaders for old saved states where iInstance was
     * 3 * sizeof(uint32_t *) due to a code mistake.
     */
    rc = SSMR3RegisterExternal(pUVM, "DisplayData", 12 /*uInstance*/, sSSMDisplayVer, 0 /*cbGuess*/,
                               NULL, NULL, NULL,
                               NULL, NULL, NULL,
                               NULL, i_displaySSMLoad, NULL, this);
    AssertRCReturn(rc, rc);

    rc = SSMR3RegisterExternal(pUVM, "DisplayData", 24 /*uInstance*/, sSSMDisplayVer, 0 /*cbGuess*/,
                               NULL, NULL, NULL,
                               NULL, NULL, NULL,
                               NULL, i_displaySSMLoad, NULL, this);
    AssertRCReturn(rc, rc);

    /* uInstance is an arbitrary value greater than 1024. Such a value will ensure a quick seek in saved state file. */
    rc = SSMR3RegisterExternal(pUVM, "DisplayScreenshot", 1100 /*uInstance*/, sSSMDisplayScreenshotVer, 0 /*cbGuess*/,
                               NULL, NULL, NULL,
                               NULL, i_displaySSMSaveScreenshot, NULL,
                               NULL, i_displaySSMLoadScreenshot, NULL, this);

    AssertRCReturn(rc, rc);

    return VINF_SUCCESS;
}

DECLCALLBACK(void) Display::i_displayCrCmdFree(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd, int rc, void *pvCompletion)
{
    Assert(pvCompletion);
    RTMemFree(pvCompletion);
}

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
int Display::i_crOglWindowsShow(bool fShow)
{
    if (!mfCrOglDataHidden == !!fShow)
        return VINF_SUCCESS;

    if (!mhCrOglSvc)
    {
        /* no 3D */
#ifdef DEBUG
        BOOL is3denabled;
        mParent->i_machine()->COMGETTER(Accelerate3DEnabled)(&is3denabled);
        Assert(!is3denabled);
#endif
        return VERR_INVALID_STATE;
    }

    VMMDev *pVMMDev = mParent->i_getVMMDev();
    if (!pVMMDev)
    {
        AssertMsgFailed(("no vmmdev\n"));
        return VERR_INVALID_STATE;
    }

    VBOXCRCMDCTL_HGCM *pData = (VBOXCRCMDCTL_HGCM*)RTMemAlloc(sizeof(VBOXCRCMDCTL_HGCM));
    if (!pData)
    {
        AssertMsgFailed(("RTMemAlloc failed\n"));
        return VERR_NO_MEMORY;
    }

    pData->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
    pData->Hdr.u32Function = SHCRGL_HOST_FN_WINDOWS_SHOW;

    pData->aParms[0].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[0].u.uint32 = (uint32_t)fShow;

    int rc = i_crCtlSubmit(&pData->Hdr, sizeof(*pData), i_displayCrCmdFree, pData);
    if (RT_SUCCESS(rc))
        mfCrOglDataHidden = !fShow;
    else
    {
        AssertMsgFailed(("crCtlSubmit failed rc %d\n", rc));
        RTMemFree(pData);
    }

    return rc;
}
#endif


// public methods only for internal purposes
/////////////////////////////////////////////////////////////////////////////

int Display::i_notifyCroglResize(const PVBVAINFOVIEW pView, const PVBVAINFOSCREEN pScreen, void *pvVRAM)
{
#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    if (maFramebuffers[pScreen->u32ViewIndex].fRenderThreadMode)
        return VINF_SUCCESS; /* nop it */

    BOOL is3denabled;
    mParent->i_machine()->COMGETTER(Accelerate3DEnabled)(&is3denabled);

    if (is3denabled)
    {
        int rc = VERR_INVALID_STATE;
        if (mhCrOglSvc)
        {
            VMMDev *pVMMDev = mParent->i_getVMMDev();
            if (pVMMDev)
            {
                VBOXCRCMDCTL_HGCM *pCtl =
                    (VBOXCRCMDCTL_HGCM*)RTMemAlloc(sizeof(CRVBOXHGCMDEVRESIZE) + sizeof(VBOXCRCMDCTL_HGCM));
                if (pCtl)
                {
                    CRVBOXHGCMDEVRESIZE *pData = (CRVBOXHGCMDEVRESIZE*)(pCtl+1);
                    pData->Screen = *pScreen;
                    pData->pvVRAM = pvVRAM;

                    pCtl->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
                    pCtl->Hdr.u32Function = SHCRGL_HOST_FN_DEV_RESIZE;
                    pCtl->aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
                    pCtl->aParms[0].u.pointer.addr = pData;
                    pCtl->aParms[0].u.pointer.size = sizeof(*pData);

                    rc = i_crCtlSubmit(&pCtl->Hdr, sizeof(*pCtl), i_displayCrCmdFree, pCtl);
                    if (!RT_SUCCESS(rc))
                    {
                        AssertMsgFailed(("crCtlSubmit failed rc %d\n", rc));
                        RTMemFree(pCtl);
                    }
                }
                else
                    rc = VERR_NO_MEMORY;
            }
        }

        return rc;
    }
#endif /* #if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL) */
    return VINF_SUCCESS;
}

/**
 *  Handles display resize event.
 *
 *  @param w New display width
 *  @param h New display height
 *
 *  @thread EMT
 */
int Display::i_handleDisplayResize(unsigned uScreenId, uint32_t bpp, void *pvVRAM,
                                   uint32_t cbLine, uint32_t w, uint32_t h, uint16_t flags)
{
    LogRel(("Display::handleDisplayResize(): uScreenId = %d, pvVRAM=%p "
            "w=%d h=%d bpp=%d cbLine=0x%X, flags=0x%X\n",
            uScreenId, pvVRAM, w, h, bpp, cbLine, flags));

    if (uScreenId >= mcMonitors)
    {
        return VINF_SUCCESS;
    }

    DISPLAYFBINFO *pFBInfo = &maFramebuffers[uScreenId];

    /* Reset the update mode. */
    pFBInfo->updateImage.pSourceBitmap.setNull();
    pFBInfo->updateImage.pu8Address = NULL;
    pFBInfo->updateImage.cbLine = 0;

    if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
    {
        pFBInfo->w = w;
        pFBInfo->h = h;

        pFBInfo->u16BitsPerPixel = (uint16_t)bpp;
        pFBInfo->pu8FramebufferVRAM = (uint8_t *)pvVRAM;
        pFBInfo->u32LineSize = cbLine;
        pFBInfo->flags = flags;
    }

    /* Guest screen image will be invalid during resize, make sure that it is not updated. */
    if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
    {
        mpDrv->pUpPort->pfnSetRenderVRAM(mpDrv->pUpPort, false);

        mpDrv->IConnector.pu8Data    = NULL;
        mpDrv->IConnector.cbScanline = 0;
        mpDrv->IConnector.cBits      = 32; /* DevVGA does not work with cBits == 0. */
        mpDrv->IConnector.cx         = 0;
        mpDrv->IConnector.cy         = 0;
    }

    maFramebuffers[uScreenId].pSourceBitmap.setNull();

    if (!maFramebuffers[uScreenId].pFramebuffer.isNull())
    {
        HRESULT hr = maFramebuffers[uScreenId].pFramebuffer->NotifyChange(uScreenId, 0, 0, w, h); /* @todo origin */
        LogFunc(("NotifyChange hr %08X\n", hr));
        NOREF(hr);
    }

    i_handleResizeCompletedEMT(uScreenId, TRUE);

    bool fUpdateImage = RT_BOOL(pFBInfo->u32Caps & FramebufferCapabilities_UpdateImage);
    if (fUpdateImage && !pFBInfo->pFramebuffer.isNull())
    {
        ComPtr<IDisplaySourceBitmap> pSourceBitmap;
        HRESULT hr = QuerySourceBitmap(uScreenId, pSourceBitmap.asOutParam());
        if (SUCCEEDED(hr))
        {
            BYTE *pAddress = NULL;
            ULONG ulWidth = 0;
            ULONG ulHeight = 0;
            ULONG ulBitsPerPixel = 0;
            ULONG ulBytesPerLine = 0;
            ULONG ulPixelFormat = 0;

            hr = pSourceBitmap->QueryBitmapInfo(&pAddress,
                                                &ulWidth,
                                                &ulHeight,
                                                &ulBitsPerPixel,
                                                &ulBytesPerLine,
                                                &ulPixelFormat);
            if (SUCCEEDED(hr))
            {
                pFBInfo->updateImage.pSourceBitmap = pSourceBitmap;
                pFBInfo->updateImage.pu8Address = pAddress;
                pFBInfo->updateImage.cbLine = ulBytesPerLine;
            }
        }
    }

    return VINF_SUCCESS;
}

/**
 *  Framebuffer has been resized.
 *  Read the new display data and unlock the framebuffer.
 *
 *  @thread EMT
 */
void Display::i_handleResizeCompletedEMT(unsigned uScreenId, BOOL fResizeContext)
{
    LogRelFlowFunc(("\n"));

    do /* to use 'break' */
    {
        if (uScreenId >= mcMonitors)
        {
            break;
        }

        DISPLAYFBINFO *pFBInfo = &maFramebuffers[uScreenId];

        /* Inform VRDP server about the change of display parameters.
         * Must be done before calling NotifyUpdate below.
         */
        LogRelFlowFunc(("Calling VRDP\n"));
        mParent->i_consoleVRDPServer()->SendResize();

        /* @todo Merge these two 'if's within one 'if (!pFBInfo->pFramebuffer.isNull())' */
        if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN && !pFBInfo->pFramebuffer.isNull())
        {
            /* If the screen resize was because of disabling, tell framebuffer to repaint.
             * The framebuffer if now in default format so it will not use guest VRAM
             * and will show usually black image which is there after framebuffer resize.
             */
            if (pFBInfo->fDisabled)
                pFBInfo->pFramebuffer->NotifyUpdate(0, 0, mpDrv->IConnector.cx, mpDrv->IConnector.cy);
        }
        else if (!pFBInfo->pFramebuffer.isNull())
        {
            /* If the screen resize was because of disabling, tell framebuffer to repaint.
             * The framebuffer if now in default format so it will not use guest VRAM
             * and will show usually black image which is there after framebuffer resize.
             */
            if (pFBInfo->fDisabled)
                pFBInfo->pFramebuffer->NotifyUpdate(0, 0, pFBInfo->w, pFBInfo->h);
        }
        LogRelFlow(("[%d]: default format %d\n", uScreenId, pFBInfo->fDefaultFormat));
    } while(0);
}

static void i_checkCoordBounds(int *px, int *py, int *pw, int *ph, int cx, int cy)
{
    /* Correct negative x and y coordinates. */
    if (*px < 0)
    {
        *px += *pw; /* Compute xRight which is also the new width. */

        *pw = (*px < 0)? 0: *px;

        *px = 0;
    }

    if (*py < 0)
    {
        *py += *ph; /* Compute xBottom, which is also the new height. */

        *ph = (*py < 0)? 0: *py;

        *py = 0;
    }

    /* Also check if coords are greater than the display resolution. */
    if (*px + *pw > cx)
    {
        *pw = cx > *px? cx - *px: 0;
    }

    if (*py + *ph > cy)
    {
        *ph = cy > *py? cy - *py: 0;
    }
}

void Display::i_handleDisplayUpdate(unsigned uScreenId, int x, int y, int w, int h)
{
    /*
     * Always runs under either VBVA lock or, for HGSMI, DevVGA lock.
     * Safe to use VBVA vars and take the framebuffer lock.
     */

#ifdef DEBUG_sunlover
    LogFlowFunc(("[%d] %d,%d %dx%d (%d,%d)\n",
                 uScreenId, x, y, w, h, mpDrv->IConnector.cx, mpDrv->IConnector.cy));
#endif /* DEBUG_sunlover */

    /* No updates for a disabled guest screen. */
    if (maFramebuffers[uScreenId].fDisabled)
        return;

    if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
        i_checkCoordBounds (&x, &y, &w, &h, mpDrv->IConnector.cx, mpDrv->IConnector.cy);
    else
        i_checkCoordBounds (&x, &y, &w, &h, maFramebuffers[uScreenId].w,
                                          maFramebuffers[uScreenId].h);

    IFramebuffer *pFramebuffer = maFramebuffers[uScreenId].pFramebuffer;
    if (pFramebuffer != NULL)
    {
        if (w != 0 && h != 0)
        {
            bool fUpdateImage = RT_BOOL(maFramebuffers[uScreenId].u32Caps & FramebufferCapabilities_UpdateImage);
            if (RT_LIKELY(!fUpdateImage))
            {
                pFramebuffer->NotifyUpdate(x, y, w, h);
            }
            else
            {
                AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

                DISPLAYFBINFO *pFBInfo = &maFramebuffers[uScreenId];

                if (!pFBInfo->updateImage.pSourceBitmap.isNull())
                {
                    Assert(pFBInfo->updateImage.pu8Address);

                    size_t cbData = w * h * 4;
                    com::SafeArray<BYTE> image(cbData);

                    uint8_t *pu8Dst = image.raw();
                    const uint8_t *pu8Src = pFBInfo->updateImage.pu8Address + pFBInfo->updateImage.cbLine * y + x * 4;

                    int i;
                    for (i = y; i < y + h; ++i)
                    {
                        memcpy(pu8Dst, pu8Src, w * 4);
                        pu8Dst += w * 4;
                        pu8Src += pFBInfo->updateImage.cbLine;
                    }

                    pFramebuffer->NotifyUpdateImage(x, y, w, h, ComSafeArrayAsInParam(image));
                }
            }
        }
    }

#ifndef VBOX_WITH_HGSMI
    if (!mVideoAccelLegacy.fVideoAccelEnabled)
    {
#else
    if (!mVideoAccelLegacy.fVideoAccelEnabled && !maFramebuffers[uScreenId].fVBVAEnabled)
    {
#endif /* VBOX_WITH_HGSMI */
        /* When VBVA is enabled, the VRDP server is informed
         * either in VideoAccelFlush or displayVBVAUpdateProcess.
         * Inform the server here only if VBVA is disabled.
         */
        mParent->i_consoleVRDPServer()->SendUpdateBitmap(uScreenId, x, y, w, h);
    }
}

/**
 * Returns the upper left and lower right corners of the virtual framebuffer.
 * The lower right is "exclusive" (i.e. first pixel beyond the framebuffer),
 * and the origin is (0, 0), not (1, 1) like the GUI returns.
 */
void Display::i_getFramebufferDimensions(int32_t *px1, int32_t *py1,
                                         int32_t *px2, int32_t *py2)
{
    int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    AssertPtrReturnVoid(px1);
    AssertPtrReturnVoid(py1);
    AssertPtrReturnVoid(px2);
    AssertPtrReturnVoid(py2);
    LogRelFlowFunc(("\n"));

    if (!mpDrv)
        return;
    /* If VBVA is not in use then this flag will not be set and this
     * will still work as it should. */
    if (!maFramebuffers[0].fDisabled)
    {
        x1 = (int32_t)maFramebuffers[0].xOrigin;
        y1 = (int32_t)maFramebuffers[0].yOrigin;
        x2 = mpDrv->IConnector.cx + (int32_t)maFramebuffers[0].xOrigin;
        y2 = mpDrv->IConnector.cy + (int32_t)maFramebuffers[0].yOrigin;
    }
    for (unsigned i = 1; i < mcMonitors; ++i)
    {
        if (!maFramebuffers[i].fDisabled)
        {
            x1 = RT_MIN(x1, maFramebuffers[i].xOrigin);
            y1 = RT_MIN(y1, maFramebuffers[i].yOrigin);
            x2 = RT_MAX(x2,   maFramebuffers[i].xOrigin
                            + (int32_t)maFramebuffers[i].w);
            y2 = RT_MAX(y2,   maFramebuffers[i].yOrigin
                            + (int32_t)maFramebuffers[i].h);
        }
    }
    *px1 = x1;
    *py1 = y1;
    *px2 = x2;
    *py2 = y2;
}

static bool displayIntersectRect(RTRECT *prectResult,
                                 const RTRECT *prect1,
                                 const RTRECT *prect2)
{
    /* Initialize result to an empty record. */
    memset(prectResult, 0, sizeof(RTRECT));

    int xLeftResult = RT_MAX(prect1->xLeft, prect2->xLeft);
    int xRightResult = RT_MIN(prect1->xRight, prect2->xRight);

    if (xLeftResult < xRightResult)
    {
        /* There is intersection by X. */

        int yTopResult = RT_MAX(prect1->yTop, prect2->yTop);
        int yBottomResult = RT_MIN(prect1->yBottom, prect2->yBottom);

        if (yTopResult < yBottomResult)
        {
            /* There is intersection by Y. */

            prectResult->xLeft   = xLeftResult;
            prectResult->yTop    = yTopResult;
            prectResult->xRight  = xRightResult;
            prectResult->yBottom = yBottomResult;

            return true;
        }
    }

    return false;
}

int Display::i_handleSetVisibleRegion(uint32_t cRect, PRTRECT pRect)
{
    RTRECT *pVisibleRegion = (RTRECT *)RTMemTmpAlloc(  RT_MAX(cRect, 1)
                                                     * sizeof(RTRECT));
    if (!pVisibleRegion)
    {
        return VERR_NO_TMP_MEMORY;
    }

    unsigned uScreenId;
    for (uScreenId = 0; uScreenId < mcMonitors; uScreenId++)
    {
        DISPLAYFBINFO *pFBInfo = &maFramebuffers[uScreenId];

        if (  !pFBInfo->pFramebuffer.isNull()
            & RT_BOOL(pFBInfo->u32Caps & FramebufferCapabilities_VisibleRegion))
        {
            /* Prepare a new array of rectangles which intersect with the framebuffer.
             */
            RTRECT rectFramebuffer;
            if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
            {
                rectFramebuffer.xLeft   = 0;
                rectFramebuffer.yTop    = 0;
                if (mpDrv)
                {
                    rectFramebuffer.xRight  = mpDrv->IConnector.cx;
                    rectFramebuffer.yBottom = mpDrv->IConnector.cy;
                }
                else
                {
                    rectFramebuffer.xRight  = 0;
                    rectFramebuffer.yBottom = 0;
                }
            }
            else
            {
                rectFramebuffer.xLeft   = pFBInfo->xOrigin;
                rectFramebuffer.yTop    = pFBInfo->yOrigin;
                rectFramebuffer.xRight  = pFBInfo->xOrigin + pFBInfo->w;
                rectFramebuffer.yBottom = pFBInfo->yOrigin + pFBInfo->h;
            }

            uint32_t cRectVisibleRegion = 0;

            uint32_t i;
            for (i = 0; i < cRect; i++)
            {
                if (displayIntersectRect(&pVisibleRegion[cRectVisibleRegion], &pRect[i], &rectFramebuffer))
                {
                    pVisibleRegion[cRectVisibleRegion].xLeft -= pFBInfo->xOrigin;
                    pVisibleRegion[cRectVisibleRegion].yTop -= pFBInfo->yOrigin;
                    pVisibleRegion[cRectVisibleRegion].xRight -= pFBInfo->xOrigin;
                    pVisibleRegion[cRectVisibleRegion].yBottom -= pFBInfo->yOrigin;

                    cRectVisibleRegion++;
                }
            }
            pFBInfo->pFramebuffer->SetVisibleRegion((BYTE *)pVisibleRegion, cRectVisibleRegion);
        }
    }

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    BOOL is3denabled = FALSE;

    mParent->i_machine()->COMGETTER(Accelerate3DEnabled)(&is3denabled);

    VMMDev *vmmDev = mParent->i_getVMMDev();
    if (is3denabled && vmmDev)
    {
        if (mhCrOglSvc)
        {
            VBOXCRCMDCTL_HGCM *pCtl = (VBOXCRCMDCTL_HGCM*)RTMemAlloc(RT_MAX(cRect, 1) * sizeof(RTRECT)
                    + sizeof(VBOXCRCMDCTL_HGCM));
            if (pCtl)
            {
                RTRECT *pRectsCopy = (RTRECT*)(pCtl+1);
                memcpy(pRectsCopy, pRect, cRect * sizeof(RTRECT));

                pCtl->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
                pCtl->Hdr.u32Function = SHCRGL_HOST_FN_SET_VISIBLE_REGION;

                pCtl->aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
                pCtl->aParms[0].u.pointer.addr = pRectsCopy;
                pCtl->aParms[0].u.pointer.size = cRect * sizeof(RTRECT);

                int rc = i_crCtlSubmit(&pCtl->Hdr, sizeof(*pCtl), i_displayCrCmdFree, pCtl);
                if (!RT_SUCCESS(rc))
                {
                    AssertMsgFailed(("crCtlSubmit failed rc %d\n", rc));
                    RTMemFree(pCtl);
                }
            }
            else
                AssertMsgFailed(("failed to allocate rects memory\n"));
        }
        else
            AssertMsgFailed(("mhCrOglSvc is NULL\n"));
    }
#endif

    RTMemTmpFree(pVisibleRegion);

    return VINF_SUCCESS;
}

int Display::i_handleQueryVisibleRegion(uint32_t *pcRect, PRTRECT pRect)
{
    // @todo Currently not used by the guest and is not implemented in framebuffers. Remove?
    return VERR_NOT_SUPPORTED;
}

#ifdef VBOX_WITH_HGSMI
static void vbvaSetMemoryFlagsHGSMI(unsigned uScreenId,
                                    uint32_t fu32SupportedOrders,
                                    bool fVideoAccelVRDP,
                                    DISPLAYFBINFO *pFBInfo)
{
    LogRelFlowFunc(("HGSMI[%d]: %p\n", uScreenId, pFBInfo->pVBVAHostFlags));

    if (pFBInfo->pVBVAHostFlags)
    {
        uint32_t fu32HostEvents = VBOX_VIDEO_INFO_HOST_EVENTS_F_VRDP_RESET;

        if (pFBInfo->fVBVAEnabled)
        {
            fu32HostEvents |= VBVA_F_MODE_ENABLED;

            if (fVideoAccelVRDP)
            {
                fu32HostEvents |= VBVA_F_MODE_VRDP;
            }
        }

        ASMAtomicWriteU32(&pFBInfo->pVBVAHostFlags->u32HostEvents, fu32HostEvents);
        ASMAtomicWriteU32(&pFBInfo->pVBVAHostFlags->u32SupportedOrders, fu32SupportedOrders);

        LogRelFlowFunc(("    fu32HostEvents = 0x%08X, fu32SupportedOrders = 0x%08X\n", fu32HostEvents, fu32SupportedOrders));
    }
}

static void vbvaSetMemoryFlagsAllHGSMI(uint32_t fu32SupportedOrders,
                                       bool fVideoAccelVRDP,
                                       DISPLAYFBINFO *paFBInfos,
                                       unsigned cFBInfos)
{
    unsigned uScreenId;

    for (uScreenId = 0; uScreenId < cFBInfos; uScreenId++)
    {
        vbvaSetMemoryFlagsHGSMI(uScreenId, fu32SupportedOrders, fVideoAccelVRDP, &paFBInfos[uScreenId]);
    }
}
#endif /* VBOX_WITH_HGSMI */

int Display::VideoAccelEnableVMMDev(bool fEnable, VBVAMEMORY *pVbvaMemory)
{
    LogFlowFunc(("%d %p\n", fEnable, pVbvaMemory));
    int rc = videoAccelEnterVMMDev(&mVideoAccelLegacy);
    if (RT_SUCCESS(rc))
    {
        rc = i_VideoAccelEnable(fEnable, pVbvaMemory, mpDrv->pUpPort);
        videoAccelLeaveVMMDev(&mVideoAccelLegacy);
    }
    LogFlowFunc(("leave %Rrc\n", rc));
    return rc;
}

int Display::VideoAccelEnableVGA(bool fEnable, VBVAMEMORY *pVbvaMemory)
{
    LogFlowFunc(("%d %p\n", fEnable, pVbvaMemory));
    int rc = videoAccelEnterVGA(&mVideoAccelLegacy);
    if (RT_SUCCESS(rc))
    {
        rc = i_VideoAccelEnable(fEnable, pVbvaMemory, mpDrv->pUpPort);
        videoAccelLeaveVGA(&mVideoAccelLegacy);
    }
    LogFlowFunc(("leave %Rrc\n", rc));
    return rc;
}

void Display::VideoAccelFlushVMMDev(void)
{
    LogFlowFunc(("enter\n"));
    int rc = videoAccelEnterVMMDev(&mVideoAccelLegacy);
    if (RT_SUCCESS(rc))
    {
        i_VideoAccelFlush(mpDrv->pUpPort);
        videoAccelLeaveVMMDev(&mVideoAccelLegacy);
    }
    LogFlowFunc(("leave\n"));
}

/* Called always by one VRDP server thread. Can be thread-unsafe.
 */
void Display::i_VideoAccelVRDP(bool fEnable)
{
    LogRelFlowFunc(("fEnable = %d\n", fEnable));

    VIDEOACCEL *pVideoAccel = &mVideoAccelLegacy;

    int c = fEnable?
                ASMAtomicIncS32(&mcVideoAccelVRDPRefs):
                ASMAtomicDecS32(&mcVideoAccelVRDPRefs);

    Assert (c >= 0);

    /* This can run concurrently with Display videoaccel state change. */
    RTCritSectEnter(&mVideoAccelLock);

    if (c == 0)
    {
        /* The last client has disconnected, and the accel can be
         * disabled.
         */
        Assert (fEnable == false);

        mfVideoAccelVRDP = false;
        mfu32SupportedOrders = 0;

        i_vbvaSetMemoryFlags(pVideoAccel->pVbvaMemory, pVideoAccel->fVideoAccelEnabled, mfVideoAccelVRDP, mfu32SupportedOrders,
                             maFramebuffers, mcMonitors);
#ifdef VBOX_WITH_HGSMI
        /* Here is VRDP-IN thread. Process the request in vbvaUpdateBegin under DevVGA lock on an EMT. */
        ASMAtomicIncU32(&mu32UpdateVBVAFlags);
#endif /* VBOX_WITH_HGSMI */

        LogRel(("VBVA: VRDP acceleration has been disabled.\n"));
    }
    else if (   c == 1
             && !mfVideoAccelVRDP)
    {
        /* The first client has connected. Enable the accel.
         */
        Assert (fEnable == true);

        mfVideoAccelVRDP = true;
        /* Supporting all orders. */
        mfu32SupportedOrders = ~0;

        i_vbvaSetMemoryFlags(pVideoAccel->pVbvaMemory, pVideoAccel->fVideoAccelEnabled, mfVideoAccelVRDP, mfu32SupportedOrders,
                             maFramebuffers, mcMonitors);
#ifdef VBOX_WITH_HGSMI
        /* Here is VRDP-IN thread. Process the request in vbvaUpdateBegin under DevVGA lock on an EMT. */
        ASMAtomicIncU32(&mu32UpdateVBVAFlags);
#endif /* VBOX_WITH_HGSMI */

        LogRel(("VBVA: VRDP acceleration has been requested.\n"));
    }
    else
    {
        /* A client is connected or disconnected but there is no change in the
         * accel state. It remains enabled.
         */
        Assert(mfVideoAccelVRDP == true);
    }

    RTCritSectLeave(&mVideoAccelLock);
}

void Display::i_notifyPowerDown(void)
{
    LogRelFlowFunc(("\n"));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    /* Source bitmaps are not available anymore. */
    mfSourceBitmapEnabled = false;

    /* Resize all displays to tell framebuffers to forget current source bitmap. */
    unsigned uScreenId = mcMonitors;
    while (uScreenId > 0)
    {
        --uScreenId;

        DISPLAYFBINFO *pFBInfo = &maFramebuffers[uScreenId];
        if (!pFBInfo->fDisabled)
        {
            i_handleDisplayResize(uScreenId, 32,
                                  pFBInfo->pu8FramebufferVRAM,
                                  pFBInfo->u32LineSize,
                                  pFBInfo->w,
                                  pFBInfo->h,
                                  pFBInfo->flags);
        }
    }
}

// Wrapped IDisplay methods
/////////////////////////////////////////////////////////////////////////////
HRESULT Display::getScreenResolution(ULONG aScreenId, ULONG *aWidth, ULONG *aHeight, ULONG *aBitsPerPixel,
                                     LONG *aXOrigin, LONG *aYOrigin)
{
    LogRelFlowFunc(("aScreenId=%RU32\n", aScreenId));

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    uint32_t u32Width = 0;
    uint32_t u32Height = 0;
    uint32_t u32BitsPerPixel = 0;
    int32_t xOrigin = 0;
    int32_t yOrigin = 0;

    if (aScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
    {
        if (mpDrv)
        {
            u32Width = mpDrv->IConnector.cx;
            u32Height = mpDrv->IConnector.cy;

            alock.release();

            int rc = mpDrv->pUpPort->pfnQueryColorDepth(mpDrv->pUpPort, &u32BitsPerPixel);
            AssertRC(rc);

            alock.acquire();
        }
    }
    else if (aScreenId < mcMonitors)
    {
        DISPLAYFBINFO *pFBInfo = &maFramebuffers[aScreenId];
        u32Width = pFBInfo->w;
        u32Height = pFBInfo->h;
        u32BitsPerPixel = pFBInfo->u16BitsPerPixel;
        xOrigin = pFBInfo->xOrigin;
        yOrigin = pFBInfo->yOrigin;
    }
    else
    {
        return E_INVALIDARG;
    }

    if (aWidth)
        *aWidth = u32Width;
    if (aHeight)
        *aHeight = u32Height;
    if (aBitsPerPixel)
        *aBitsPerPixel = u32BitsPerPixel;
    if (aXOrigin)
        *aXOrigin = xOrigin;
    if (aYOrigin)
        *aYOrigin = yOrigin;

    return S_OK;
}


HRESULT Display::attachFramebuffer(ULONG aScreenId, const ComPtr<IFramebuffer> &aFramebuffer)
{
    LogRelFlowFunc(("aScreenId = %d\n", aScreenId));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (aScreenId >= mcMonitors)
        return setError(E_INVALIDARG, tr("AttachFramebuffer: Invalid screen %d (total %d)"),
                        aScreenId, mcMonitors);

    DISPLAYFBINFO *pFBInfo = &maFramebuffers[aScreenId];
    if (!pFBInfo->pFramebuffer.isNull())
        return setError(E_FAIL, tr("AttachFramebuffer: Framebuffer already attached to %d"),
                        aScreenId);

    pFBInfo->pFramebuffer = aFramebuffer;

    SafeArray<FramebufferCapabilities_T> caps;
    pFBInfo->pFramebuffer->COMGETTER(Capabilities)(ComSafeArrayAsOutParam(caps));
    pFBInfo->u32Caps = 0;
    size_t i;
    for (i = 0; i < caps.size(); ++i)
        pFBInfo->u32Caps |= caps[i];

    alock.release();

    /* The driver might not have been constructed yet */
    if (mpDrv)
    {
        /* Setup the new framebuffer. */
        i_handleDisplayResize(aScreenId, pFBInfo->u16BitsPerPixel,
                              pFBInfo->pu8FramebufferVRAM,
                              pFBInfo->u32LineSize,
                              pFBInfo->w,
                              pFBInfo->h,
                              pFBInfo->flags);
    }

    Console::SafeVMPtrQuiet ptrVM(mParent);
    if (ptrVM.isOk())
    {
#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
        BOOL fIs3DEnabled = FALSE;
        mParent->i_machine()->COMGETTER(Accelerate3DEnabled)(&fIs3DEnabled);

        if (fIs3DEnabled)
        {
            VBOXCRCMDCTL_HGCM data;
            RT_ZERO(data);
            data.Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
            data.Hdr.u32Function = SHCRGL_HOST_FN_SCREEN_CHANGED;

            data.aParms[0].type = VBOX_HGCM_SVC_PARM_32BIT;
            data.aParms[0].u.uint32 = aScreenId;

            int vrc = i_crCtlSubmitSync(&data.Hdr, sizeof(data));
            AssertRC(vrc);
        }
#endif /* defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL) */

        VMR3ReqCallNoWaitU(ptrVM.rawUVM(), VMCPUID_ANY, (PFNRT)Display::i_InvalidateAndUpdateEMT,
                           3, this, aScreenId, false);
    }

    LogRelFlowFunc(("Attached to %d\n", aScreenId));
    return S_OK;
}

HRESULT Display::detachFramebuffer(ULONG aScreenId)
{
    LogRelFlowFunc(("aScreenId = %d\n", aScreenId));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (aScreenId >= mcMonitors)
        return setError(E_INVALIDARG, tr("DetachFramebuffer: Invalid screen %d (total %d)"),
                        aScreenId, mcMonitors);

    DISPLAYFBINFO *pFBInfo = &maFramebuffers[aScreenId];

    pFBInfo->pFramebuffer.setNull();

    alock.release();

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    Console::SafeVMPtrQuiet ptrVM(mParent);
    if (ptrVM.isOk())
    {
        BOOL fIs3DEnabled = FALSE;
        mParent->i_machine()->COMGETTER(Accelerate3DEnabled)(&fIs3DEnabled);

        if (fIs3DEnabled)
        {
            VBOXCRCMDCTL_HGCM data;
            RT_ZERO(data);
            data.Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
            data.Hdr.u32Function = SHCRGL_HOST_FN_SCREEN_CHANGED;

            data.aParms[0].type = VBOX_HGCM_SVC_PARM_32BIT;
            data.aParms[0].u.uint32 = aScreenId;

            int vrc = i_crCtlSubmitSync(&data.Hdr, sizeof(data));
            AssertRC(vrc);
        }
    }
#endif /* defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL) */

    return S_OK;
}

HRESULT Display::queryFramebuffer(ULONG aScreenId, ComPtr<IFramebuffer> &aFramebuffer)
{
    LogRelFlowFunc(("aScreenId = %d\n", aScreenId));

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (aScreenId >= mcMonitors)
        return setError(E_INVALIDARG, tr("QueryFramebuffer: Invalid screen %d (total %d)"),
                        aScreenId, mcMonitors);

    DISPLAYFBINFO *pFBInfo = &maFramebuffers[aScreenId];

    pFBInfo->pFramebuffer.queryInterfaceTo(aFramebuffer.asOutParam());

    return S_OK;
}

HRESULT Display::setVideoModeHint(ULONG aDisplay, BOOL aEnabled,
                                  BOOL aChangeOrigin, LONG aOriginX, LONG aOriginY,
                                  ULONG aWidth, ULONG aHeight, ULONG aBitsPerPixel)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    CHECK_CONSOLE_DRV(mpDrv);

    /*
     * Do some rough checks for valid input.
     */
    ULONG width  = aWidth;
    if (!width)
        width    = mpDrv->IConnector.cx;
    ULONG height = aHeight;
    if (!height)
        height   = mpDrv->IConnector.cy;
    ULONG bpp    = aBitsPerPixel;
    if (!bpp)
    {
        alock.release();

        uint32_t cBits = 0;
        int rc = mpDrv->pUpPort->pfnQueryColorDepth(mpDrv->pUpPort, &cBits);
        AssertRC(rc);
        bpp = cBits;

        alock.acquire();
    }
    ULONG cMonitors;
    mParent->i_machine()->COMGETTER(MonitorCount)(&cMonitors);
    if (cMonitors == 0 && aDisplay > 0)
        return E_INVALIDARG;
    if (aDisplay >= cMonitors)
        return E_INVALIDARG;

   /*
    * sunlover 20070614: It is up to the guest to decide whether the hint is
    * valid. Therefore don't do any VRAM sanity checks here!
    */

    /* Have to release the lock because the pfnRequestDisplayChange
     * will call EMT.  */
    alock.release();

    VMMDev *pVMMDev = mParent->i_getVMMDev();
    if (pVMMDev)
    {
        PPDMIVMMDEVPORT pVMMDevPort = pVMMDev->getVMMDevPort();
        if (pVMMDevPort)
            pVMMDevPort->pfnRequestDisplayChange(pVMMDevPort, aWidth, aHeight, aBitsPerPixel,
                                                 aDisplay, aOriginX, aOriginY,
                                                 RT_BOOL(aEnabled), RT_BOOL(aChangeOrigin));
    }
    return S_OK;
}

HRESULT Display::setSeamlessMode(BOOL enabled)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    /* Have to release the lock because the pfnRequestSeamlessChange will call EMT.  */
    alock.release();

    VMMDev *pVMMDev = mParent->i_getVMMDev();
    if (pVMMDev)
    {
        PPDMIVMMDEVPORT pVMMDevPort = pVMMDev->getVMMDevPort();
        if (pVMMDevPort)
            pVMMDevPort->pfnRequestSeamlessChange(pVMMDevPort, !!enabled);
    }

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    if (!enabled)
    {
        BOOL is3denabled = FALSE;

        mParent->i_machine()->COMGETTER(Accelerate3DEnabled)(&is3denabled);

        VMMDev *vmmDev = mParent->i_getVMMDev();
        if (is3denabled && vmmDev)
        {
            VBOXCRCMDCTL_HGCM *pData = (VBOXCRCMDCTL_HGCM*)RTMemAlloc(sizeof(VBOXCRCMDCTL_HGCM));
            if (!pData)
            {
                AssertMsgFailed(("RTMemAlloc failed\n"));
                return VERR_NO_MEMORY;
            }

            pData->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
            pData->Hdr.u32Function = SHCRGL_HOST_FN_SET_VISIBLE_REGION;

            pData->aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
            pData->aParms[0].u.pointer.addr = NULL;
            pData->aParms[0].u.pointer.size = 0; /* <- means null rects, NULL pRects address and 0 rects means "disable" */

            int rc = i_crCtlSubmit(&pData->Hdr, sizeof(*pData), i_displayCrCmdFree, pData);
            if (!RT_SUCCESS(rc))
            {
                AssertMsgFailed(("crCtlSubmit failed rc %d\n", rc));
                RTMemFree(pData);
            }
        }
    }
#endif
    return S_OK;
}

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
BOOL Display::i_displayCheckTakeScreenshotCrOgl(Display *pDisplay, ULONG aScreenId, uint8_t *pu8Data,
                                                uint32_t u32Width, uint32_t u32Height)
{
    BOOL is3denabled;
    pDisplay->mParent->i_machine()->COMGETTER(Accelerate3DEnabled)(&is3denabled);
    if (is3denabled && pDisplay->mCrOglCallbacks.pfnHasData())
    {
        VMMDev *pVMMDev = pDisplay->mParent->i_getVMMDev();
        if (pVMMDev)
        {
            CRVBOXHGCMTAKESCREENSHOT *pScreenshot = (CRVBOXHGCMTAKESCREENSHOT*)RTMemAlloc(sizeof(*pScreenshot));
            if (pScreenshot)
            {
                /* screen id or CRSCREEN_ALL to specify all enabled */
                pScreenshot->u32Screen = aScreenId;
                pScreenshot->u32Width = u32Width;
                pScreenshot->u32Height = u32Height;
                pScreenshot->u32Pitch = u32Width * 4;
                pScreenshot->pvBuffer = pu8Data;
                pScreenshot->pvContext = NULL;
                pScreenshot->pfnScreenshotBegin = NULL;
                pScreenshot->pfnScreenshotPerform = NULL;
                pScreenshot->pfnScreenshotEnd = NULL;

                VBOXCRCMDCTL_HGCM data;
                data.Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
                data.Hdr.u32Function = SHCRGL_HOST_FN_TAKE_SCREENSHOT;

                data.aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
                data.aParms[0].u.pointer.addr = pScreenshot;
                data.aParms[0].u.pointer.size = sizeof(*pScreenshot);

                int rc = pDisplay->i_crCtlSubmitSync(&data.Hdr, sizeof(data));

                RTMemFree(pScreenshot);

                if (RT_SUCCESS(rc))
                    return TRUE;
                else
                {
                    AssertMsgFailed(("failed to get screenshot data from crOgl %d\n", rc));
                    /* fall back to the non-3d mechanism */
                }
            }
        }
    }
    return FALSE;
}
#endif

int Display::i_displayTakeScreenshotEMT(Display *pDisplay, ULONG aScreenId, uint8_t **ppu8Data, size_t *pcbData,
                                        uint32_t *pu32Width, uint32_t *pu32Height)
{
    int rc;

    if (   aScreenId == VBOX_VIDEO_PRIMARY_SCREEN
        && pDisplay->maFramebuffers[aScreenId].fVBVAEnabled == false) /* A non-VBVA mode. */
    {
        rc = pDisplay->mpDrv->pUpPort->pfnTakeScreenshot(pDisplay->mpDrv->pUpPort, ppu8Data, pcbData, pu32Width, pu32Height);
    }
    else if (aScreenId < pDisplay->mcMonitors)
    {
        DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[aScreenId];

        uint32_t width = pFBInfo->w;
        uint32_t height = pFBInfo->h;

        /* Allocate 32 bit per pixel bitmap. */
        size_t cbRequired = width * 4 * height;

        if (cbRequired)
        {
            uint8_t *pu8Data = (uint8_t *)RTMemAlloc(cbRequired);

            if (pu8Data == NULL)
            {
                rc = VERR_NO_MEMORY;
            }
            else
            {
                /* Copy guest VRAM to the allocated 32bpp buffer. */
                const uint8_t *pu8Src       = pFBInfo->pu8FramebufferVRAM;
                int32_t xSrc                = 0;
                int32_t ySrc                = 0;
                uint32_t u32SrcWidth        = width;
                uint32_t u32SrcHeight       = height;
                uint32_t u32SrcLineSize     = pFBInfo->u32LineSize;
                uint32_t u32SrcBitsPerPixel = pFBInfo->u16BitsPerPixel;

                uint8_t *pu8Dst             = pu8Data;
                int32_t xDst                = 0;
                int32_t yDst                = 0;
                uint32_t u32DstWidth        = u32SrcWidth;
                uint32_t u32DstHeight       = u32SrcHeight;
                uint32_t u32DstLineSize     = u32DstWidth * 4;
                uint32_t u32DstBitsPerPixel = 32;

                rc = pDisplay->mpDrv->pUpPort->pfnCopyRect(pDisplay->mpDrv->pUpPort,
                                                           width, height,
                                                           pu8Src,
                                                           xSrc, ySrc,
                                                           u32SrcWidth, u32SrcHeight,
                                                           u32SrcLineSize, u32SrcBitsPerPixel,
                                                           pu8Dst,
                                                           xDst, yDst,
                                                           u32DstWidth, u32DstHeight,
                                                           u32DstLineSize, u32DstBitsPerPixel);
                if (RT_SUCCESS(rc))
                {
                    *ppu8Data = pu8Data;
                    *pcbData = cbRequired;
                    *pu32Width = width;
                    *pu32Height = height;
                }
                else
                {
                    RTMemFree(pu8Data);

                    /* CopyRect can fail if VBVA was paused in VGA device, retry using the generic method. */
                    if (   rc == VERR_INVALID_STATE
                        && aScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
                    {
                        rc = pDisplay->mpDrv->pUpPort->pfnTakeScreenshot(pDisplay->mpDrv->pUpPort,
                                                                         ppu8Data, pcbData, pu32Width, pu32Height);
                    }
                }
            }
        }
        else
        {
            /* No image. */
            *ppu8Data = NULL;
            *pcbData = 0;
            *pu32Width = 0;
            *pu32Height = 0;
            rc = VINF_SUCCESS;
        }
    }
    else
    {
        rc = VERR_INVALID_PARAMETER;
    }

    return rc;
}

static int i_displayTakeScreenshot(PUVM pUVM, Display *pDisplay, struct DRVMAINDISPLAY *pDrv, ULONG aScreenId,
                                   BYTE *address, ULONG width, ULONG height)
{
    uint8_t *pu8Data = NULL;
    size_t cbData = 0;
    uint32_t cx = 0;
    uint32_t cy = 0;
    int vrc = VINF_SUCCESS;

# if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    if (Display::i_displayCheckTakeScreenshotCrOgl(pDisplay, aScreenId, (uint8_t*)address, width, height))
        return VINF_SUCCESS;
#endif

    int cRetries = 5;

    while (cRetries-- > 0)
    {
        /* Note! Not sure if the priority call is such a good idea here, but
                 it would be nice to have an accurate screenshot for the bug
                 report if the VM deadlocks. */
        vrc = VMR3ReqPriorityCallWaitU(pUVM, VMCPUID_ANY, (PFNRT)Display::i_displayTakeScreenshotEMT, 6,
                                       pDisplay, aScreenId, &pu8Data, &cbData, &cx, &cy);
        if (vrc != VERR_TRY_AGAIN)
        {
            break;
        }

        RTThreadSleep(10);
    }

    if (RT_SUCCESS(vrc) && pu8Data)
    {
        if (cx == width && cy == height)
        {
            /* No scaling required. */
            memcpy(address, pu8Data, cbData);
        }
        else
        {
            /* Scale. */
            LogRelFlowFunc(("SCALE: %dx%d -> %dx%d\n", cx, cy, width, height));

            uint8_t *dst = address;
            uint8_t *src = pu8Data;
            int dstW = width;
            int dstH = height;
            int srcW = cx;
            int srcH = cy;
            int iDeltaLine = cx * 4;

            BitmapScale32(dst,
                          dstW, dstH,
                          src,
                          iDeltaLine,
                          srcW, srcH);
        }

        if (aScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
        {
            /* This can be called from any thread. */
            pDrv->pUpPort->pfnFreeScreenshot(pDrv->pUpPort, pu8Data);
        }
        else
        {
            RTMemFree(pu8Data);
        }
    }

    return vrc;
}

HRESULT Display::takeScreenShotWorker(ULONG aScreenId,
                                      BYTE *aAddress,
                                      ULONG aWidth,
                                      ULONG aHeight,
                                      BitmapFormat_T aBitmapFormat,
                                      ULONG *pcbOut)
{
    HRESULT rc = S_OK;

    /* Do not allow too small and too large screenshots. This also filters out negative
     * values passed as either 'aWidth' or 'aHeight'.
     */
    CheckComArgExpr(aWidth, aWidth != 0 && aWidth <= 32767);
    CheckComArgExpr(aHeight, aHeight != 0 && aHeight <= 32767);

    if (   aBitmapFormat != BitmapFormat_BGR0
        && aBitmapFormat != BitmapFormat_BGRA
        && aBitmapFormat != BitmapFormat_RGBA
        && aBitmapFormat != BitmapFormat_PNG)
    {
        return setError(E_NOTIMPL,
                        tr("Unsupported screenshot format 0x%08X"), aBitmapFormat);
    }

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();

    int vrc = i_displayTakeScreenshot(ptrVM.rawUVM(), this, mpDrv, aScreenId, aAddress, aWidth, aHeight);

    if (RT_SUCCESS(vrc))
    {
        const size_t cbData = aWidth * 4 * aHeight;

        /* Most of uncompressed formats. */
        *pcbOut = (ULONG)cbData;

        if (aBitmapFormat == BitmapFormat_BGR0)
        {
            /* Do nothing. */
        }
        else if (aBitmapFormat == BitmapFormat_BGRA)
        {
            uint32_t *pu32 = (uint32_t *)aAddress;
            size_t cPixels = aWidth * aHeight;
            while (cPixels--)
            {
                *pu32++ |= UINT32_C(0xFF000000);
            }
        }
        else if (aBitmapFormat == BitmapFormat_RGBA)
        {
            uint8_t *pu8 = aAddress;
            size_t cPixels = aWidth * aHeight;
            while (cPixels--)
            {
                uint8_t u8 = pu8[0];
                pu8[0] = pu8[2];
                pu8[2] = u8;
                pu8[3] = 0xFF;

                pu8 += 4;
            }
        }
        else if (aBitmapFormat == BitmapFormat_PNG)
        {
            uint8_t *pu8PNG = NULL;
            uint32_t cbPNG = 0;
            uint32_t cxPNG = 0;
            uint32_t cyPNG = 0;

            vrc = DisplayMakePNG(aAddress, aWidth, aHeight, &pu8PNG, &cbPNG, &cxPNG, &cyPNG, 0);
            if (RT_SUCCESS(vrc))
            {
                if (cbPNG <= cbData)
                {
                    memcpy(aAddress, pu8PNG, cbPNG);
                    *pcbOut = cbPNG;
                }
                else
                {
                    rc = setError(E_FAIL,
                                  tr("PNG is larger than 32bpp bitmap"));
                }
            }
            else
            {
                rc = setError(VBOX_E_IPRT_ERROR,
                              tr("Could not convert screenshot to PNG (%Rrc)"), vrc);
            }
            RTMemFree(pu8PNG);
        }
    }
    else if (vrc == VERR_TRY_AGAIN)
        rc = setError(E_UNEXPECTED,
                      tr("Screenshot is not available at this time"));
    else if (RT_FAILURE(vrc))
        rc = setError(VBOX_E_IPRT_ERROR,
                      tr("Could not take a screenshot (%Rrc)"), vrc);

    return rc;
}

HRESULT Display::takeScreenShot(ULONG aScreenId,
                                BYTE *aAddress,
                                ULONG aWidth,
                                ULONG aHeight,
                                BitmapFormat_T aBitmapFormat)
{
    HRESULT rc = S_OK;

    LogRelFlowFunc(("[%d] address=%p, width=%d, height=%d, format 0x%08X\n",
                     aScreenId, aAddress, aWidth, aHeight, aBitmapFormat));

    ULONG cbOut = 0;
    rc = takeScreenShotWorker(aScreenId, aAddress, aWidth, aHeight, aBitmapFormat, &cbOut);
    NOREF(cbOut);

    LogRelFlowFunc(("%Rhrc\n", rc));
    return rc;
}

HRESULT Display::takeScreenShotToArray(ULONG aScreenId,
                                       ULONG aWidth,
                                       ULONG aHeight,
                                       BitmapFormat_T aBitmapFormat,
                                       std::vector<BYTE> &aScreenData)
{
    HRESULT rc = S_OK;

    LogRelFlowFunc(("[%d] width=%d, height=%d, format 0x%08X\n",
                     aScreenId, aWidth, aHeight, aBitmapFormat));

    /* Do not allow too small and too large screenshots. This also filters out negative
     * values passed as either 'aWidth' or 'aHeight'.
     */
    CheckComArgExpr(aWidth, aWidth != 0 && aWidth <= 32767);
    CheckComArgExpr(aHeight, aHeight != 0 && aHeight <= 32767);

    const size_t cbData = aWidth * 4 * aHeight;
    aScreenData.resize(cbData);

    ULONG cbOut = 0;
    rc = takeScreenShotWorker(aScreenId, &aScreenData.front(), aWidth, aHeight, aBitmapFormat, &cbOut);
    if (FAILED(rc))
        cbOut = 0;

    aScreenData.resize(cbOut);

    LogRelFlowFunc(("%Rhrc\n", rc));
    return rc;
}


int Display::i_VideoCaptureEnableScreens(ComSafeArrayIn(BOOL, aScreens))
{
#ifdef VBOX_WITH_VPX
    com::SafeArray<BOOL> Screens(ComSafeArrayInArg(aScreens));
    for (unsigned i = 0; i < Screens.size(); i++)
        maVideoRecEnabled[i] = RT_BOOL(Screens[i]);
    return VINF_SUCCESS;
#else
    return VERR_NOT_IMPLEMENTED;
#endif
}

/**
 * Start video capturing. Does nothing if capturing is already active.
 */
int Display::i_VideoCaptureStart()
{
#ifdef VBOX_WITH_VPX
    if (VideoRecIsEnabled(mpVideoRecCtx))
        return VINF_SUCCESS;

    int rc = VideoRecContextCreate(&mpVideoRecCtx, mcMonitors);
    if (RT_FAILURE(rc))
    {
        LogFlow(("Failed to create video recording context (%Rrc)!\n", rc));
        return rc;
    }
    ComPtr<IMachine> pMachine = mParent->i_machine();
    com::SafeArray<BOOL> screens;
    HRESULT hrc = pMachine->COMGETTER(VideoCaptureScreens)(ComSafeArrayAsOutParam(screens));
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    for (unsigned i = 0; i < RT_ELEMENTS(maVideoRecEnabled); i++)
        maVideoRecEnabled[i] = i < screens.size() && screens[i];
    ULONG ulWidth;
    hrc = pMachine->COMGETTER(VideoCaptureWidth)(&ulWidth);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    ULONG ulHeight;
    hrc = pMachine->COMGETTER(VideoCaptureHeight)(&ulHeight);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    ULONG ulRate;
    hrc = pMachine->COMGETTER(VideoCaptureRate)(&ulRate);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    ULONG ulFPS;
    hrc = pMachine->COMGETTER(VideoCaptureFPS)(&ulFPS);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    BSTR strFile;
    hrc = pMachine->COMGETTER(VideoCaptureFile)(&strFile);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    ULONG ulMaxTime;
    hrc = pMachine->COMGETTER(VideoCaptureMaxTime)(&ulMaxTime);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    ULONG ulMaxSize;
    hrc = pMachine->COMGETTER(VideoCaptureMaxFileSize)(&ulMaxSize);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    BSTR strOptions;
    hrc = pMachine->COMGETTER(VideoCaptureOptions)(&strOptions);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);

    RTTIMESPEC ts;
    RTTimeNow(&ts);
    RTTIME time;
    RTTimeExplode(&time, &ts);
    for (unsigned uScreen = 0; uScreen < mcMonitors; uScreen++)
    {
        char *pszAbsPath = RTPathAbsDup(com::Utf8Str(strFile).c_str());
        char *pszSuff = RTPathSuffix(pszAbsPath);
        if (pszSuff)
            pszSuff = RTStrDup(pszSuff);
        RTPathStripSuffix(pszAbsPath);
        if (!pszAbsPath)
            rc = VERR_INVALID_PARAMETER;
        if (!pszSuff)
            pszSuff = RTStrDup(".webm");
        char *pszName = NULL;
        if (RT_SUCCESS(rc))
        {
            if (mcMonitors > 1)
                rc = RTStrAPrintf(&pszName, "%s-%u%s", pszAbsPath, uScreen+1, pszSuff);
            else
                rc = RTStrAPrintf(&pszName, "%s%s", pszAbsPath, pszSuff);
        }
        if (RT_SUCCESS(rc))
        {
            rc = VideoRecStrmInit(mpVideoRecCtx, uScreen,
                                  pszName, ulWidth, ulHeight,
                                  ulRate, ulFPS, ulMaxTime,
                                  ulMaxSize, com::Utf8Str(strOptions).c_str());
            if (rc == VERR_ALREADY_EXISTS)
            {
                RTStrFree(pszName);
                pszName = NULL;

                if (mcMonitors > 1)
                    rc = RTStrAPrintf(&pszName, "%s-%04d-%02u-%02uT%02u-%02u-%02u-%09uZ-%u%s",
                                      pszAbsPath, time.i32Year, time.u8Month, time.u8MonthDay,
                                      time.u8Hour, time.u8Minute, time.u8Second, time.u32Nanosecond,
                                      uScreen+1, pszSuff);
                else
                    rc = RTStrAPrintf(&pszName, "%s-%04d-%02u-%02uT%02u-%02u-%02u-%09uZ%s",
                                      pszAbsPath, time.i32Year, time.u8Month, time.u8MonthDay,
                                      time.u8Hour, time.u8Minute, time.u8Second, time.u32Nanosecond,
                                      pszSuff);
                if (RT_SUCCESS(rc))
                    rc = VideoRecStrmInit(mpVideoRecCtx, uScreen,
                                          pszName, ulWidth, ulHeight, ulRate,
                                          ulFPS, ulMaxTime,
                                          ulMaxSize, com::Utf8Str(strOptions).c_str());
            }
        }

        if (RT_SUCCESS(rc))
            LogRel(("WebM/VP8 video recording screen #%u with %ux%u @ %u kbps, %u fps to '%s' enabled.\n",
                   uScreen, ulWidth, ulHeight, ulRate, ulFPS, pszName));
        else
            LogRel(("Failed to initialize video recording context #%u (%Rrc)!\n", uScreen, rc));
        RTStrFree(pszName);
        RTStrFree(pszSuff);
        RTStrFree(pszAbsPath);
    }
    return rc;
#else
    return VERR_NOT_IMPLEMENTED;
#endif
}

/**
 * Stop video capturing. Does nothing if video capturing is not active.
 */
void Display::i_VideoCaptureStop()
{
#ifdef VBOX_WITH_VPX
    if (VideoRecIsEnabled(mpVideoRecCtx))
        LogRel(("WebM/VP8 video recording stopped.\n"));
    VideoRecContextClose(mpVideoRecCtx);
    mpVideoRecCtx = NULL;
#endif
}

int Display::i_drawToScreenEMT(Display *pDisplay, ULONG aScreenId, BYTE *address,
                               ULONG x, ULONG y, ULONG width, ULONG height)
{
    int rc = VINF_SUCCESS;

    DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[aScreenId];

    if (aScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
    {
        rc = pDisplay->mpDrv->pUpPort->pfnDisplayBlt(pDisplay->mpDrv->pUpPort, address, x, y, width, height);
    }
    else if (aScreenId < pDisplay->mcMonitors)
    {
        /* Copy the bitmap to the guest VRAM. */
        const uint8_t *pu8Src       = address;
        int32_t xSrc                = 0;
        int32_t ySrc                = 0;
        uint32_t u32SrcWidth        = width;
        uint32_t u32SrcHeight       = height;
        uint32_t u32SrcLineSize     = width * 4;
        uint32_t u32SrcBitsPerPixel = 32;

        uint8_t *pu8Dst             = pFBInfo->pu8FramebufferVRAM;
        int32_t xDst                = x;
        int32_t yDst                = y;
        uint32_t u32DstWidth        = pFBInfo->w;
        uint32_t u32DstHeight       = pFBInfo->h;
        uint32_t u32DstLineSize     = pFBInfo->u32LineSize;
        uint32_t u32DstBitsPerPixel = pFBInfo->u16BitsPerPixel;

        rc = pDisplay->mpDrv->pUpPort->pfnCopyRect(pDisplay->mpDrv->pUpPort,
                                                   width, height,
                                                   pu8Src,
                                                   xSrc, ySrc,
                                                   u32SrcWidth, u32SrcHeight,
                                                   u32SrcLineSize, u32SrcBitsPerPixel,
                                                   pu8Dst,
                                                   xDst, yDst,
                                                   u32DstWidth, u32DstHeight,
                                                   u32DstLineSize, u32DstBitsPerPixel);
        if (RT_SUCCESS(rc))
        {
            if (!pFBInfo->pSourceBitmap.isNull())
            {
                /* Update the changed screen area. When source bitmap uses VRAM directly, just notify
                 * frontend to update. And for default format, render the guest VRAM to the source bitmap.
                 */
                if (   pFBInfo->fDefaultFormat
                    && !pFBInfo->fDisabled)
                {
                    BYTE *pAddress = NULL;
                    ULONG ulWidth = 0;
                    ULONG ulHeight = 0;
                    ULONG ulBitsPerPixel = 0;
                    ULONG ulBytesPerLine = 0;
                    ULONG ulPixelFormat = 0;

                    HRESULT hrc = pFBInfo->pSourceBitmap->QueryBitmapInfo(&pAddress,
                                                                          &ulWidth,
                                                                          &ulHeight,
                                                                          &ulBitsPerPixel,
                                                                          &ulBytesPerLine,
                                                                          &ulPixelFormat);
                    if (SUCCEEDED(hrc))
                    {
                        pu8Src       = pFBInfo->pu8FramebufferVRAM;
                        xSrc                = x;
                        ySrc                = y;
                        u32SrcWidth        = pFBInfo->w;
                        u32SrcHeight       = pFBInfo->h;
                        u32SrcLineSize     = pFBInfo->u32LineSize;
                        u32SrcBitsPerPixel = pFBInfo->u16BitsPerPixel;

                        /* Default format is 32 bpp. */
                        pu8Dst             = pAddress;
                        xDst                = xSrc;
                        yDst                = ySrc;
                        u32DstWidth        = u32SrcWidth;
                        u32DstHeight       = u32SrcHeight;
                        u32DstLineSize     = u32DstWidth * 4;
                        u32DstBitsPerPixel = 32;

                        pDisplay->mpDrv->pUpPort->pfnCopyRect(pDisplay->mpDrv->pUpPort,
                                                              width, height,
                                                              pu8Src,
                                                              xSrc, ySrc,
                                                              u32SrcWidth, u32SrcHeight,
                                                              u32SrcLineSize, u32SrcBitsPerPixel,
                                                              pu8Dst,
                                                              xDst, yDst,
                                                              u32DstWidth, u32DstHeight,
                                                              u32DstLineSize, u32DstBitsPerPixel);
                    }
                }
            }

            pDisplay->i_handleDisplayUpdate(aScreenId, x, y, width, height);
        }
    }
    else
    {
        rc = VERR_INVALID_PARAMETER;
    }

    if (RT_SUCCESS(rc))
        pDisplay->mParent->i_consoleVRDPServer()->SendUpdateBitmap(aScreenId, x, y, width, height);

    return rc;
}

HRESULT Display::drawToScreen(ULONG aScreenId, BYTE *aAddress, ULONG aX, ULONG aY, ULONG aWidth, ULONG aHeight)
{
    /// @todo (r=dmik) this function may take too long to complete if the VM
    //  is doing something like saving state right now. Which, in case if it
    //  is called on the GUI thread, will make it unresponsive. We should
    //  check the machine state here (by enclosing the check and VMRequCall
    //  within the Console lock to make it atomic).

    LogRelFlowFunc(("aAddress=%p, x=%d, y=%d, width=%d, height=%d\n",
                   (void *)aAddress, aX, aY, aWidth, aHeight));

    CheckComArgExpr(aWidth, aWidth != 0);
    CheckComArgExpr(aHeight, aHeight != 0);

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    CHECK_CONSOLE_DRV(mpDrv);

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();

    /* Release lock because the call scheduled on EMT may also try to take it. */
    alock.release();

    /*
     * Again we're lazy and make the graphics device do all the
     * dirty conversion work.
     */
    int rcVBox = VMR3ReqCallWaitU(ptrVM.rawUVM(), VMCPUID_ANY, (PFNRT)Display::i_drawToScreenEMT, 7,
                                  this, aScreenId, aAddress, aX, aY, aWidth, aHeight);

    /*
     * If the function returns not supported, we'll have to do all the
     * work ourselves using the framebuffer.
     */
    HRESULT rc = S_OK;
    if (rcVBox == VERR_NOT_SUPPORTED || rcVBox == VERR_NOT_IMPLEMENTED)
    {
        /** @todo implement generic fallback for screen blitting. */
        rc = E_NOTIMPL;
    }
    else if (RT_FAILURE(rcVBox))
        rc = setError(VBOX_E_IPRT_ERROR,
                      tr("Could not draw to the screen (%Rrc)"), rcVBox);
//@todo
//    else
//    {
//        /* All ok. Redraw the screen. */
//        handleDisplayUpdate (x, y, width, height);
//    }

    LogRelFlowFunc(("rc=%Rhrc\n", rc));
    return rc;
}

int Display::i_InvalidateAndUpdateEMT(Display *pDisplay, unsigned uId, bool fUpdateAll)
{
    LogRelFlowFunc(("uId=%d, fUpdateAll %d\n", uId, fUpdateAll));

    unsigned uScreenId;
    for (uScreenId = (fUpdateAll ? 0 : uId); uScreenId < pDisplay->mcMonitors; uScreenId++)
    {
        DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[uScreenId];

        if (   !pFBInfo->fVBVAEnabled
            && uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
        {
            pDisplay->mpDrv->pUpPort->pfnUpdateDisplayAll(pDisplay->mpDrv->pUpPort, /* fFailOnResize = */ true);
        }
        else
        {
            if (!pFBInfo->fDisabled)
            {
                /* Render complete VRAM screen to the framebuffer.
                 * When framebuffer uses VRAM directly, just notify it to update.
                 */
                if (pFBInfo->fDefaultFormat && !pFBInfo->pSourceBitmap.isNull())
                {
                    BYTE *pAddress = NULL;
                    ULONG ulWidth = 0;
                    ULONG ulHeight = 0;
                    ULONG ulBitsPerPixel = 0;
                    ULONG ulBytesPerLine = 0;
                    ULONG ulPixelFormat = 0;

                    HRESULT hrc = pFBInfo->pSourceBitmap->QueryBitmapInfo(&pAddress,
                                                                          &ulWidth,
                                                                          &ulHeight,
                                                                          &ulBitsPerPixel,
                                                                          &ulBytesPerLine,
                                                                          &ulPixelFormat);
                    if (SUCCEEDED(hrc))
                    {
                        uint32_t width              = pFBInfo->w;
                        uint32_t height             = pFBInfo->h;

                        const uint8_t *pu8Src       = pFBInfo->pu8FramebufferVRAM;
                        int32_t xSrc                = 0;
                        int32_t ySrc                = 0;
                        uint32_t u32SrcWidth        = pFBInfo->w;
                        uint32_t u32SrcHeight       = pFBInfo->h;
                        uint32_t u32SrcLineSize     = pFBInfo->u32LineSize;
                        uint32_t u32SrcBitsPerPixel = pFBInfo->u16BitsPerPixel;

                        /* Default format is 32 bpp. */
                        uint8_t *pu8Dst             = pAddress;
                        int32_t xDst                = xSrc;
                        int32_t yDst                = ySrc;
                        uint32_t u32DstWidth        = u32SrcWidth;
                        uint32_t u32DstHeight       = u32SrcHeight;
                        uint32_t u32DstLineSize     = u32DstWidth * 4;
                        uint32_t u32DstBitsPerPixel = 32;

                        /* if uWidth != pFBInfo->w and uHeight != pFBInfo->h
                         * implies resize of Framebuffer is in progress and
                         * copyrect should not be called.
                         */
                        if (ulWidth == pFBInfo->w && ulHeight == pFBInfo->h)
                        {
                            pDisplay->mpDrv->pUpPort->pfnCopyRect(pDisplay->mpDrv->pUpPort,
                                                                  width, height,
                                                                  pu8Src,
                                                                  xSrc, ySrc,
                                                                  u32SrcWidth, u32SrcHeight,
                                                                  u32SrcLineSize, u32SrcBitsPerPixel,
                                                                  pu8Dst,
                                                                  xDst, yDst,
                                                                  u32DstWidth, u32DstHeight,
                                                                  u32DstLineSize, u32DstBitsPerPixel);
                        }
                    }
                }

                pDisplay->i_handleDisplayUpdate(uScreenId, 0, 0, pFBInfo->w, pFBInfo->h);
            }
        }
        if (!fUpdateAll)
            break;
    }
    LogRelFlowFunc(("done\n"));
    return VINF_SUCCESS;
}

/**
 * Does a full invalidation of the VM display and instructs the VM
 * to update it immediately.
 *
 * @returns COM status code
 */

HRESULT Display::invalidateAndUpdate()
{
    LogRelFlowFunc(("\n"));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    CHECK_CONSOLE_DRV(mpDrv);

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();

    HRESULT rc = S_OK;

    LogRelFlowFunc(("Sending DPYUPDATE request\n"));

    /* Have to release the lock when calling EMT.  */
    alock.release();

    int rcVBox = VMR3ReqCallNoWaitU(ptrVM.rawUVM(), VMCPUID_ANY, (PFNRT)Display::i_InvalidateAndUpdateEMT,
                                    3, this, 0, true);
    alock.acquire();

    if (RT_FAILURE(rcVBox))
        rc = setError(VBOX_E_IPRT_ERROR,
                      tr("Could not invalidate and update the screen (%Rrc)"), rcVBox);

    LogRelFlowFunc(("rc=%Rhrc\n", rc));
    return rc;
}

HRESULT Display::invalidateAndUpdateScreen(ULONG aScreenId)
{
    LogRelFlowFunc(("\n"));

    HRESULT rc = S_OK;

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();

    int rcVBox = VMR3ReqCallNoWaitU(ptrVM.rawUVM(), VMCPUID_ANY, (PFNRT)Display::i_InvalidateAndUpdateEMT,
                                    3, this, aScreenId, false);
    if (RT_FAILURE(rcVBox))
        rc = setError(VBOX_E_IPRT_ERROR,
                      tr("Could not invalidate and update the screen %d (%Rrc)"), aScreenId, rcVBox);

    LogRelFlowFunc(("rc=%Rhrc\n", rc));
    return rc;
}

HRESULT Display::completeVHWACommand(BYTE *aCommand)
{
#ifdef VBOX_WITH_VIDEOHWACCEL
    mpDrv->pVBVACallbacks->pfnVHWACommandCompleteAsync(mpDrv->pVBVACallbacks, (PVBOXVHWACMD)aCommand);
    return S_OK;
#else
    return E_NOTIMPL;
#endif
}

HRESULT Display::viewportChanged(ULONG aScreenId, ULONG aX, ULONG aY, ULONG aWidth, ULONG aHeight)
{
#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)

    if (mcMonitors <= aScreenId)
    {
        AssertMsgFailed(("invalid screen id\n"));
        return E_INVALIDARG;
    }

    BOOL is3denabled;
    mParent->i_machine()->COMGETTER(Accelerate3DEnabled)(&is3denabled);

    if (is3denabled)
    {
        int rc = i_crViewportNotify(aScreenId, aX, aY, aWidth, aHeight);
        if (RT_FAILURE(rc))
        {
            DISPLAYFBINFO *pFb = &maFramebuffers[aScreenId];
            pFb->pendingViewportInfo.fPending = true;
            pFb->pendingViewportInfo.x = aX;
            pFb->pendingViewportInfo.y = aY;
            pFb->pendingViewportInfo.width = aWidth;
            pFb->pendingViewportInfo.height = aHeight;
        }
    }
#endif /* VBOX_WITH_CROGL && VBOX_WITH_HGCM */
    return S_OK;
}

HRESULT Display::querySourceBitmap(ULONG aScreenId,
                                   ComPtr<IDisplaySourceBitmap> &aDisplaySourceBitmap)
{
    LogRelFlowFunc(("aScreenId = %d\n", aScreenId));

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();

    bool fSetRenderVRAM = false;
    bool fInvalidate = false;

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (aScreenId >= mcMonitors)
        return setError(E_INVALIDARG, tr("QuerySourceBitmap: Invalid screen %d (total %d)"),
                        aScreenId, mcMonitors);

    if (!mfSourceBitmapEnabled)
    {
        aDisplaySourceBitmap = NULL;
        return E_FAIL;
    }

    HRESULT hr = S_OK;

    DISPLAYFBINFO *pFBInfo = &maFramebuffers[aScreenId];
    if (pFBInfo->pSourceBitmap.isNull())
    {
        /* Create a new object. */
        ComObjPtr<DisplaySourceBitmap> obj;
        hr = obj.createObject();
        if (SUCCEEDED(hr))
            hr = obj->init(this, aScreenId, pFBInfo);

        if (SUCCEEDED(hr))
        {
            bool fDefaultFormat = !obj->i_usesVRAM();

            if (aScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
            {
                /* Start buffer updates. */
                BYTE *pAddress = NULL;
                ULONG ulWidth = 0;
                ULONG ulHeight = 0;
                ULONG ulBitsPerPixel = 0;
                ULONG ulBytesPerLine = 0;
                ULONG ulPixelFormat = 0;

                obj->QueryBitmapInfo(&pAddress,
                                     &ulWidth,
                                     &ulHeight,
                                     &ulBitsPerPixel,
                                     &ulBytesPerLine,
                                     &ulPixelFormat);

                mpDrv->IConnector.pu8Data    = pAddress;
                mpDrv->IConnector.cbScanline = ulBytesPerLine;
                mpDrv->IConnector.cBits      = ulBitsPerPixel;
                mpDrv->IConnector.cx         = ulWidth;
                mpDrv->IConnector.cy         = ulHeight;

                fSetRenderVRAM = fDefaultFormat;
            }

            /* Make sure that the bitmap contains the latest image. */
            fInvalidate = fDefaultFormat;

            pFBInfo->pSourceBitmap = obj;
            pFBInfo->fDefaultFormat = fDefaultFormat;
        }
    }

    if (SUCCEEDED(hr))
    {
        pFBInfo->pSourceBitmap.queryInterfaceTo(aDisplaySourceBitmap.asOutParam());
    }

    /* Leave the IDisplay lock because the VGA device must not be called under it. */
    alock.release();

    if (SUCCEEDED(hr))
    {
        if (fSetRenderVRAM)
        {
            mpDrv->pUpPort->pfnSetRenderVRAM(mpDrv->pUpPort, true);
        }

        if (fInvalidate)
            VMR3ReqCallWaitU(ptrVM.rawUVM(), VMCPUID_ANY, (PFNRT)Display::i_InvalidateAndUpdateEMT,
                             3, this, aScreenId, false);
    }

    LogRelFlowFunc(("%Rhrc\n", hr));
    return hr;
}

// wrapped IEventListener method
HRESULT Display::handleEvent(const ComPtr<IEvent> &aEvent)
{
    VBoxEventType_T aType = VBoxEventType_Invalid;

    aEvent->COMGETTER(Type)(&aType);
    switch (aType)
    {
        case VBoxEventType_OnStateChanged:
        {
            ComPtr<IStateChangedEvent> scev = aEvent;
            Assert(scev);
            MachineState_T machineState;
            scev->COMGETTER(State)(&machineState);
            if (   machineState == MachineState_Running
                || machineState == MachineState_Teleporting
                || machineState == MachineState_LiveSnapshotting
                || machineState == MachineState_DeletingSnapshotOnline
                   )
            {
                LogRelFlowFunc(("Machine is running.\n"));

#ifdef VBOX_WITH_CROGL
                i_crOglWindowsShow(true);
#endif
            }
            else
            {
#ifdef VBOX_WITH_CROGL
                if (machineState == MachineState_Paused)
                    i_crOglWindowsShow(false);
#endif
            }
            break;
        }
        default:
            AssertFailed();
    }

    return S_OK;
}


// private methods
/////////////////////////////////////////////////////////////////////////////

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
int Display::i_crViewportNotify(ULONG aScreenId, ULONG x, ULONG y, ULONG width, ULONG height)
{
    VMMDev *pVMMDev = mParent->i_getVMMDev();
    if (!pVMMDev)
        return VERR_INVALID_STATE;

    size_t cbData = RT_UOFFSETOF(VBOXCRCMDCTL_HGCM, aParms[5]);
    VBOXCRCMDCTL_HGCM *pData = (VBOXCRCMDCTL_HGCM*)alloca(cbData);

    pData->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
    pData->Hdr.u32Function = SHCRGL_HOST_FN_VIEWPORT_CHANGED;

    pData->aParms[0].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[0].u.uint32 = aScreenId;

    pData->aParms[1].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[1].u.uint32 = x;

    pData->aParms[2].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[2].u.uint32 = y;

    pData->aParms[3].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[3].u.uint32 = width;

    pData->aParms[4].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[4].u.uint32 = height;

    return i_crCtlSubmitSyncIfHasDataForScreen(aScreenId, &pData->Hdr, (uint32_t)cbData);
}
#endif

#ifdef VBOX_WITH_CRHGSMI
void Display::i_setupCrHgsmiData(void)
{
    VMMDev *pVMMDev = mParent->i_getVMMDev();
    Assert(pVMMDev);
    int rc = RTCritSectRwEnterExcl(&mCrOglLock);
    AssertRC(rc);

    if (pVMMDev)
        rc = pVMMDev->hgcmHostSvcHandleCreate("VBoxSharedCrOpenGL", &mhCrOglSvc);
    else
        rc = VERR_GENERAL_FAILURE;

    if (RT_SUCCESS(rc))
    {
        Assert(mhCrOglSvc);
        /* setup command completion callback */
        VBOXVDMACMD_CHROMIUM_CTL_CRHGSMI_SETUP_MAINCB Completion;
        Completion.Hdr.enmType = VBOXVDMACMD_CHROMIUM_CTL_TYPE_CRHGSMI_SETUP_MAINCB;
        Completion.Hdr.cbCmd = sizeof(Completion);
        Completion.hCompletion = mpDrv->pVBVACallbacks;
        Completion.pfnCompletion = mpDrv->pVBVACallbacks->pfnCrHgsmiCommandCompleteAsync;

        VBOXHGCMSVCPARM parm;
        parm.type = VBOX_HGCM_SVC_PARM_PTR;
        parm.u.pointer.addr = &Completion;
        parm.u.pointer.size = 0;

        rc = pVMMDev->hgcmHostCall("VBoxSharedCrOpenGL", SHCRGL_HOST_FN_CRHGSMI_CTL, 1, &parm);
        if (RT_SUCCESS(rc))
            mCrOglCallbacks = Completion.MainInterface;
        else
            AssertMsgFailed(("VBOXVDMACMD_CHROMIUM_CTL_TYPE_CRHGSMI_SETUP_COMPLETION failed rc %d", rc));
    }

    if (RT_FAILURE(rc))
        mhCrOglSvc = NULL;

    RTCritSectRwLeaveExcl(&mCrOglLock);
}

void Display::i_destructCrHgsmiData(void)
{
    int rc = RTCritSectRwEnterExcl(&mCrOglLock);
    AssertRC(rc);
    mhCrOglSvc = NULL;
    RTCritSectRwLeaveExcl(&mCrOglLock);
}
#endif

/**
 * Handle display resize event issued by the VGA device for the primary screen.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnResize
 */
DECLCALLBACK(int) Display::i_displayResizeCallback(PPDMIDISPLAYCONNECTOR pInterface,
                                                   uint32_t bpp, void *pvVRAM, uint32_t cbLine, uint32_t cx, uint32_t cy)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;

    LogRelFlowFunc(("bpp %d, pvVRAM %p, cbLine %d, cx %d, cy %d\n",
                  bpp, pvVRAM, cbLine, cx, cy));

    bool f = ASMAtomicCmpXchgBool(&pThis->fVGAResizing, true, false);
    if (!f)
    {
        /* This is a result of recursive call when the source bitmap is being updated
         * during a VGA resize. Tell the VGA device to ignore the call.
         *
         * @todo It is a workaround, actually pfnUpdateDisplayAll must
         * fail on resize.
         */
        LogRel(("displayResizeCallback: already processing\n"));
        return VINF_VGA_RESIZE_IN_PROGRESS;
    }

    int rc = pThis->i_handleDisplayResize(VBOX_VIDEO_PRIMARY_SCREEN, bpp, pvVRAM, cbLine, cx, cy, VBVA_SCREEN_F_ACTIVE);

    /* Restore the flag.  */
    f = ASMAtomicCmpXchgBool(&pThis->fVGAResizing, false, true);
    AssertRelease(f);

    return rc;
}

/**
 * Handle display update.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnUpdateRect
 */
DECLCALLBACK(void) Display::i_displayUpdateCallback(PPDMIDISPLAYCONNECTOR pInterface,
                                                    uint32_t x, uint32_t y, uint32_t cx, uint32_t cy)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

#ifdef DEBUG_sunlover
    LogFlowFunc(("fVideoAccelEnabled = %d, %d,%d %dx%d\n",
                 pDrv->pDisplay->mVideoAccelLegacy.fVideoAccelEnabled, x, y, cx, cy));
#endif /* DEBUG_sunlover */

    /* This call does update regardless of VBVA status.
     * But in VBVA mode this is called only as result of
     * pfnUpdateDisplayAll in the VGA device.
     */

    pDrv->pDisplay->i_handleDisplayUpdate(VBOX_VIDEO_PRIMARY_SCREEN, x, y, cx, cy);
}

/**
 * Periodic display refresh callback.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnRefresh
 * @thread EMT
 */
DECLCALLBACK(void) Display::i_displayRefreshCallback(PPDMIDISPLAYCONNECTOR pInterface)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

#ifdef DEBUG_sunlover_2
    LogFlowFunc(("pDrv->pDisplay->mfVideoAccelEnabled = %d\n",
                 pDrv->pDisplay->mfVideoAccelEnabled));
#endif /* DEBUG_sunlover_2 */

    Display *pDisplay = pDrv->pDisplay;
    unsigned uScreenId;

    int rc = pDisplay->i_videoAccelRefreshProcess(pDrv->pUpPort);
    if (rc != VINF_TRY_AGAIN) /* Means 'do nothing' here. */
    {
        if (rc == VWRN_INVALID_STATE)
        {
            /* No VBVA do a display update. */
            DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[VBOX_VIDEO_PRIMARY_SCREEN];
            pDrv->pUpPort->pfnUpdateDisplay(pDrv->pUpPort);
        }

        /* Inform the VRDP server that the current display update sequence is
         * completed. At this moment the framebuffer memory contains a definite
         * image, that is synchronized with the orders already sent to VRDP client.
         * The server can now process redraw requests from clients or initial
         * fullscreen updates for new clients.
         */
        for (uScreenId = 0; uScreenId < pDisplay->mcMonitors; uScreenId++)
        {
            DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[uScreenId];

            Assert(pDisplay->mParent && pDisplay->mParent->i_consoleVRDPServer());
            pDisplay->mParent->i_consoleVRDPServer()->SendUpdate(uScreenId, NULL, 0);
        }
    }

#ifdef VBOX_WITH_VPX
    if (VideoRecIsEnabled(pDisplay->mpVideoRecCtx))
    {
        do {
# if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
            BOOL is3denabled;
            pDisplay->mParent->i_machine()->COMGETTER(Accelerate3DEnabled)(&is3denabled);
            if (is3denabled)
            {
                if (ASMAtomicCmpXchgU32(&pDisplay->mfCrOglVideoRecState, CRVREC_STATE_SUBMITTED, CRVREC_STATE_IDLE))
                {
                    if (pDisplay->mCrOglCallbacks.pfnHasData())
                    {
                        /* submit */
                        VBOXCRCMDCTL_HGCM *pData = &pDisplay->mCrOglScreenshotCtl;

                        pData->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
                        pData->Hdr.u32Function = SHCRGL_HOST_FN_TAKE_SCREENSHOT;

                        pData->aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
                        pData->aParms[0].u.pointer.addr = &pDisplay->mCrOglScreenshotData;
                        pData->aParms[0].u.pointer.size = sizeof(pDisplay->mCrOglScreenshotData);
                        rc = pDisplay->i_crCtlSubmit(&pData->Hdr, sizeof(*pData), Display::i_displayVRecCompletion, pDisplay);
                        if (RT_SUCCESS(rc))
                            break;
                        else
                            AssertMsgFailed(("crCtlSubmit failed rc %d\n", rc));
                    }

                    /* no 3D data available, or error has occured,
                     * go the straight way */
                    ASMAtomicWriteU32(&pDisplay->mfCrOglVideoRecState, CRVREC_STATE_IDLE);
                }
                else
                {
                    /* record request is still in progress, don't do anything */
                    break;
                }
            }
# endif /* VBOX_WITH_HGCM && VBOX_WITH_CROGL */

            uint64_t u64Now = RTTimeProgramMilliTS();
            for (uScreenId = 0; uScreenId < pDisplay->mcMonitors; uScreenId++)
            {
                if (!pDisplay->maVideoRecEnabled[uScreenId])
                    continue;

                if (VideoRecIsFull(pDisplay->mpVideoRecCtx, uScreenId, u64Now))
                {
                    pDisplay->i_VideoCaptureStop();
                    pDisplay->mParent->i_machine()->COMSETTER(VideoCaptureEnabled)(false);
                    break;
                }

                DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[uScreenId];

                if (   !pFBInfo->pFramebuffer.isNull()
                    && !pFBInfo->fDisabled)
                {
                    rc = VERR_NOT_SUPPORTED;
                    if (   pFBInfo->fVBVAEnabled
                        && pFBInfo->pu8FramebufferVRAM)
                    {
                        rc = VideoRecCopyToIntBuf(pDisplay->mpVideoRecCtx, uScreenId, 0, 0,
                                                  BitmapFormat_BGR,
                                                  pFBInfo->u16BitsPerPixel,
                                                  pFBInfo->u32LineSize, pFBInfo->w, pFBInfo->h,
                                                  pFBInfo->pu8FramebufferVRAM, u64Now);
                    }
                    else if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN && pDrv->IConnector.pu8Data)
                    {
                        rc = VideoRecCopyToIntBuf(pDisplay->mpVideoRecCtx, uScreenId, 0, 0,
                                                  BitmapFormat_BGR,
                                                  pDrv->IConnector.cBits,
                                                  pDrv->IConnector.cbScanline, pDrv->IConnector.cx,
                                                  pDrv->IConnector.cy, pDrv->IConnector.pu8Data, u64Now);
                    }
                    if (rc == VINF_TRY_AGAIN)
                        break;
                }
            }
        } while (0);
    }
#endif /* VBOX_WITH_VPX */

#ifdef DEBUG_sunlover_2
    LogFlowFunc(("leave\n"));
#endif /* DEBUG_sunlover_2 */
}

/**
 * Reset notification
 *
 * @see PDMIDISPLAYCONNECTOR::pfnReset
 */
DECLCALLBACK(void) Display::i_displayResetCallback(PPDMIDISPLAYCONNECTOR pInterface)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    LogRelFlowFunc(("\n"));

   /* Disable VBVA mode. */
    pDrv->pDisplay->VideoAccelEnableVGA(false, NULL);
}

/**
 * LFBModeChange notification
 *
 * @see PDMIDISPLAYCONNECTOR::pfnLFBModeChange
 */
DECLCALLBACK(void) Display::i_displayLFBModeChangeCallback(PPDMIDISPLAYCONNECTOR pInterface, bool fEnabled)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    LogRelFlowFunc(("fEnabled=%d\n", fEnabled));

    NOREF(fEnabled);

    /* Disable VBVA mode in any case. The guest driver reenables VBVA mode if necessary. */
    pDrv->pDisplay->VideoAccelEnableVGA(false, NULL);
}

/**
 * Adapter information change notification.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnProcessAdapterData
 */
DECLCALLBACK(void) Display::i_displayProcessAdapterDataCallback(PPDMIDISPLAYCONNECTOR pInterface, void *pvVRAM,
                                                                uint32_t u32VRAMSize)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    pDrv->pDisplay->processAdapterData(pvVRAM, u32VRAMSize);
}

/**
 * Display information change notification.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnProcessDisplayData
 */
DECLCALLBACK(void) Display::i_displayProcessDisplayDataCallback(PPDMIDISPLAYCONNECTOR pInterface,
                                                                void *pvVRAM, unsigned uScreenId)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    pDrv->pDisplay->processDisplayData(pvVRAM, uScreenId);
}

#ifdef VBOX_WITH_VIDEOHWACCEL

#ifndef S_FALSE
# define S_FALSE ((HRESULT)1L)
#endif

int Display::i_handleVHWACommandProcess(PVBOXVHWACMD pCommand)
{
    unsigned id = (unsigned)pCommand->iDisplay;
    int rc = VINF_SUCCESS;
    if (id >= mcMonitors)
        return VERR_INVALID_PARAMETER;

    ComPtr<IFramebuffer> pFramebuffer;
    AutoReadLock arlock(this COMMA_LOCKVAL_SRC_POS);
    pFramebuffer = maFramebuffers[id].pFramebuffer;
    bool fVHWASupported = RT_BOOL(maFramebuffers[id].u32Caps & FramebufferCapabilities_VHWA);
    arlock.release();

    if (pFramebuffer == NULL || !fVHWASupported)
        return VERR_NOT_IMPLEMENTED; /* Implementation is not available. */

    HRESULT hr = pFramebuffer->ProcessVHWACommand((BYTE*)pCommand);
    if (hr == S_FALSE)
        return VINF_SUCCESS;
    else if (SUCCEEDED(hr))
        return VINF_CALLBACK_RETURN;
    else if (hr == E_ACCESSDENIED)
        return VERR_INVALID_STATE; /* notify we can not handle request atm */
    else if (hr == E_NOTIMPL)
        return VERR_NOT_IMPLEMENTED;
    return VERR_GENERAL_FAILURE;
}

DECLCALLBACK(int) Display::i_displayVHWACommandProcess(PPDMIDISPLAYCONNECTOR pInterface, PVBOXVHWACMD pCommand)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    return pDrv->pDisplay->i_handleVHWACommandProcess(pCommand);
}
#endif

#ifdef VBOX_WITH_CRHGSMI
void Display::i_handleCrHgsmiCommandCompletion(int32_t result, uint32_t u32Function, PVBOXHGCMSVCPARM pParam)
{
    mpDrv->pVBVACallbacks->pfnCrHgsmiCommandCompleteAsync(mpDrv->pVBVACallbacks,
                                                          (PVBOXVDMACMD_CHROMIUM_CMD)pParam->u.pointer.addr, result);
}

void Display::i_handleCrHgsmiControlCompletion(int32_t result, uint32_t u32Function, PVBOXHGCMSVCPARM pParam)
{
    PVBOXVDMACMD_CHROMIUM_CTL pCtl = (PVBOXVDMACMD_CHROMIUM_CTL)pParam->u.pointer.addr;
    mpDrv->pVBVACallbacks->pfnCrHgsmiControlCompleteAsync(mpDrv->pVBVACallbacks, pCtl, result);
}

void Display::i_handleCrHgsmiCommandProcess(PVBOXVDMACMD_CHROMIUM_CMD pCmd, uint32_t cbCmd)
{
    int rc = VERR_NOT_SUPPORTED;
    VBOXHGCMSVCPARM parm;
    parm.type = VBOX_HGCM_SVC_PARM_PTR;
    parm.u.pointer.addr = pCmd;
    parm.u.pointer.size = cbCmd;

    if (mhCrOglSvc)
    {
        VMMDev *pVMMDev = mParent->i_getVMMDev();
        if (pVMMDev)
        {
            /* no completion callback is specified with this call,
             * the CrOgl code will complete the CrHgsmi command once it processes it */
            rc = pVMMDev->hgcmHostFastCallAsync(mhCrOglSvc, SHCRGL_HOST_FN_CRHGSMI_CMD, &parm, NULL, NULL);
            AssertRC(rc);
            if (RT_SUCCESS(rc))
                return;
        }
        else
            rc = VERR_INVALID_STATE;
    }

    /* we are here because something went wrong with command processing, complete it */
    i_handleCrHgsmiCommandCompletion(rc, SHCRGL_HOST_FN_CRHGSMI_CMD, &parm);
}

void Display::i_handleCrHgsmiControlProcess(PVBOXVDMACMD_CHROMIUM_CTL pCtl, uint32_t cbCtl)
{
    int rc = VERR_NOT_SUPPORTED;
    VBOXHGCMSVCPARM parm;
    parm.type = VBOX_HGCM_SVC_PARM_PTR;
    parm.u.pointer.addr = pCtl;
    parm.u.pointer.size = cbCtl;

    if (mhCrOglSvc)
    {
        VMMDev *pVMMDev = mParent->i_getVMMDev();
        if (pVMMDev)
        {
            bool fCheckPendingViewport = (pCtl->enmType == VBOXVDMACMD_CHROMIUM_CTL_TYPE_CRHGSMI_SETUP);
            rc = pVMMDev->hgcmHostFastCallAsync(mhCrOglSvc, SHCRGL_HOST_FN_CRHGSMI_CTL, &parm,
                                                Display::i_displayCrHgsmiControlCompletion, this);
            AssertRC(rc);
            if (RT_SUCCESS(rc))
            {
                if (fCheckPendingViewport)
                {
                    ULONG ul;
                    for (ul = 0; ul < mcMonitors; ul++)
                    {
                        DISPLAYFBINFO *pFb = &maFramebuffers[ul];
                        if (!pFb->pendingViewportInfo.fPending)
                            continue;

                        rc = i_crViewportNotify(ul, pFb->pendingViewportInfo.x, pFb->pendingViewportInfo.y,
                                              pFb->pendingViewportInfo.width, pFb->pendingViewportInfo.height);
                        if (RT_SUCCESS(rc))
                            pFb->pendingViewportInfo.fPending = false;
                        else
                        {
                            AssertMsgFailed(("crViewportNotify failed %d\n", rc));
                            rc = VINF_SUCCESS;
                        }
                    }
                }
                return;
            }
        }
        else
            rc = VERR_INVALID_STATE;
    }

    /* we are here because something went wrong with command processing, complete it */
    i_handleCrHgsmiControlCompletion(rc, SHCRGL_HOST_FN_CRHGSMI_CTL, &parm);
}

DECLCALLBACK(void) Display::i_displayCrHgsmiCommandProcess(PPDMIDISPLAYCONNECTOR pInterface, PVBOXVDMACMD_CHROMIUM_CMD pCmd,
                                                           uint32_t cbCmd)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    pDrv->pDisplay->i_handleCrHgsmiCommandProcess(pCmd, cbCmd);
}

DECLCALLBACK(void) Display::i_displayCrHgsmiControlProcess(PPDMIDISPLAYCONNECTOR pInterface, PVBOXVDMACMD_CHROMIUM_CTL pCmd,
                                                           uint32_t cbCmd)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    pDrv->pDisplay->i_handleCrHgsmiControlProcess(pCmd, cbCmd);
}

DECLCALLBACK(void) Display::i_displayCrHgsmiCommandCompletion(int32_t result, uint32_t u32Function, PVBOXHGCMSVCPARM pParam,
                                                              void *pvContext)
{
    AssertMsgFailed(("not expected!"));
    Display *pDisplay = (Display *)pvContext;
    pDisplay->i_handleCrHgsmiCommandCompletion(result, u32Function, pParam);
}

DECLCALLBACK(void) Display::i_displayCrHgsmiControlCompletion(int32_t result, uint32_t u32Function, PVBOXHGCMSVCPARM pParam,
                                                              void *pvContext)
{
    Display *pDisplay = (Display *)pvContext;
    pDisplay->i_handleCrHgsmiControlCompletion(result, u32Function, pParam);

}
#endif

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
DECLCALLBACK(void)  Display::i_displayCrHgcmCtlSubmitCompletion(int32_t result, uint32_t u32Function, PVBOXHGCMSVCPARM pParam,
                                                                void *pvContext)
{
    VBOXCRCMDCTL *pCmd = (VBOXCRCMDCTL*)pParam->u.pointer.addr;
    if (pCmd->u.pfnInternal)
        ((PFNCRCTLCOMPLETION)pCmd->u.pfnInternal)(pCmd, pParam->u.pointer.size, result, pvContext);
}

int  Display::i_handleCrHgcmCtlSubmit(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd,
                                    PFNCRCTLCOMPLETION pfnCompletion,
                                    void *pvCompletion)
{
    VMMDev *pVMMDev = mParent ? mParent->i_getVMMDev() : NULL;
    if (!pVMMDev)
    {
        AssertMsgFailed(("no vmmdev\n"));
        return VERR_INVALID_STATE;
    }

    Assert(mhCrOglSvc);
    VBOXHGCMSVCPARM parm;
    parm.type = VBOX_HGCM_SVC_PARM_PTR;
    parm.u.pointer.addr = pCmd;
    parm.u.pointer.size = cbCmd;

    pCmd->u.pfnInternal = (void(*)())pfnCompletion;
    int rc = pVMMDev->hgcmHostFastCallAsync(mhCrOglSvc, SHCRGL_HOST_FN_CTL, &parm, i_displayCrHgcmCtlSubmitCompletion,
                                            pvCompletion);
    if (!RT_SUCCESS(rc))
        AssertMsgFailed(("hgcmHostFastCallAsync failed rc %d\n", rc));

    return rc;
}

DECLCALLBACK(int)  Display::i_displayCrHgcmCtlSubmit(PPDMIDISPLAYCONNECTOR pInterface,
                                                     struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd,
                                                     PFNCRCTLCOMPLETION pfnCompletion,
                                                     void *pvCompletion)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;
    return pThis->i_handleCrHgcmCtlSubmit(pCmd, cbCmd, pfnCompletion, pvCompletion);
}

int Display::i_crCtlSubmit(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd, PFNCRCTLCOMPLETION pfnCompletion, void *pvCompletion)
{
    int rc = RTCritSectRwEnterShared(&mCrOglLock);
    if (RT_SUCCESS(rc))
    {
        if (mhCrOglSvc)
            rc = mpDrv->pVBVACallbacks->pfnCrCtlSubmit(mpDrv->pVBVACallbacks, pCmd, cbCmd, pfnCompletion, pvCompletion);
        else
            rc = VERR_NOT_SUPPORTED;

        RTCritSectRwLeaveShared(&mCrOglLock);
    }
    return rc;
}

int Display::i_crCtlSubmitSync(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd)
{
    int rc = RTCritSectRwEnterShared(&mCrOglLock);
    if (RT_SUCCESS(rc))
    {
        if (mhCrOglSvc)
            rc = mpDrv->pVBVACallbacks->pfnCrCtlSubmitSync(mpDrv->pVBVACallbacks, pCmd, cbCmd);
        else
            rc = VERR_NOT_SUPPORTED;

        RTCritSectRwLeaveShared(&mCrOglLock);
    }
    return rc;
}

int Display::i_crCtlSubmitAsyncCmdCopy(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd)
{
    VBOXCRCMDCTL* pCmdCopy = (VBOXCRCMDCTL*)RTMemAlloc(cbCmd);
    if (!pCmdCopy)
    {
        LogRel(("RTMemAlloc failed\n"));
        return VERR_NO_MEMORY;
    }

    memcpy(pCmdCopy, pCmd, cbCmd);

    int rc = i_crCtlSubmit(pCmdCopy, cbCmd, i_displayCrCmdFree, pCmdCopy);
    if (RT_FAILURE(rc))
    {
        LogRel(("crCtlSubmit failed %d\n", rc));
        RTMemFree(pCmdCopy);
        return rc;
    }

    return VINF_SUCCESS;
}

int Display::i_crCtlSubmitSyncIfHasDataForScreen(uint32_t u32ScreenID, struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd)
{
    int rc = RTCritSectRwEnterShared(&mCrOglLock);
    AssertRCReturn(rc, rc);

    if (mCrOglCallbacks.pfnHasDataForScreen && mCrOglCallbacks.pfnHasDataForScreen(u32ScreenID))
        rc = i_crCtlSubmitSync(pCmd, cbCmd);
    else
        rc = i_crCtlSubmitAsyncCmdCopy(pCmd, cbCmd);

    RTCritSectRwLeaveShared(&mCrOglLock);

    return rc;
}

bool  Display::i_handleCrVRecScreenshotBegin(uint32_t uScreen, uint64_t u64TimeStamp)
{
# if VBOX_WITH_VPX
    return VideoRecIsReady(mpVideoRecCtx, uScreen, u64TimeStamp);
# else
    return false;
# endif
}

void  Display::i_handleCrVRecScreenshotEnd(uint32_t uScreen, uint64_t u64TimeStamp)
{
}

void  Display::i_handleCrVRecScreenshotPerform(uint32_t uScreen,
                                               uint32_t x, uint32_t y, uint32_t uPixelFormat,
                                               uint32_t uBitsPerPixel, uint32_t uBytesPerLine,
                                               uint32_t uGuestWidth, uint32_t uGuestHeight,
                                               uint8_t *pu8BufferAddress, uint64_t u64TimeStamp)
{
    Assert(mfCrOglVideoRecState == CRVREC_STATE_SUBMITTED);
# if VBOX_WITH_VPX
    int rc = VideoRecCopyToIntBuf(mpVideoRecCtx, uScreen, x, y,
                                  uPixelFormat,
                                  uBitsPerPixel, uBytesPerLine,
                                  uGuestWidth, uGuestHeight,
                                  pu8BufferAddress, u64TimeStamp);
    Assert(rc == VINF_SUCCESS /* || rc == VERR_TRY_AGAIN || rc == VINF_TRY_AGAIN*/);
# endif
}

void  Display::i_handleVRecCompletion()
{
    Assert(mfCrOglVideoRecState == CRVREC_STATE_SUBMITTED);
    ASMAtomicWriteU32(&mfCrOglVideoRecState, CRVREC_STATE_IDLE);
}

DECLCALLBACK(void) Display::i_displayCrVRecScreenshotPerform(void *pvCtx, uint32_t uScreen,
                                                             uint32_t x, uint32_t y,
                                                             uint32_t uBitsPerPixel, uint32_t uBytesPerLine,
                                                             uint32_t uGuestWidth, uint32_t uGuestHeight,
                                                             uint8_t *pu8BufferAddress, uint64_t u64TimeStamp)
{
    Display *pDisplay = (Display *)pvCtx;
    pDisplay->i_handleCrVRecScreenshotPerform(uScreen,
                                              x, y, BitmapFormat_BGR, uBitsPerPixel,
                                              uBytesPerLine, uGuestWidth, uGuestHeight,
                                              pu8BufferAddress, u64TimeStamp);
}

DECLCALLBACK(bool) Display::i_displayCrVRecScreenshotBegin(void *pvCtx, uint32_t uScreen, uint64_t u64TimeStamp)
{
    Display *pDisplay = (Display *)pvCtx;
    return pDisplay->i_handleCrVRecScreenshotBegin(uScreen, u64TimeStamp);
}

DECLCALLBACK(void) Display::i_displayCrVRecScreenshotEnd(void *pvCtx, uint32_t uScreen, uint64_t u64TimeStamp)
{
    Display *pDisplay = (Display *)pvCtx;
    pDisplay->i_handleCrVRecScreenshotEnd(uScreen, u64TimeStamp);
}

DECLCALLBACK(void) Display::i_displayVRecCompletion(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd, int rc, void *pvCompletion)
{
    Display *pDisplay = (Display *)pvCompletion;
    pDisplay->i_handleVRecCompletion();
}

#endif


#ifdef VBOX_WITH_HGSMI
DECLCALLBACK(int) Display::i_displayVBVAEnable(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId, PVBVAHOSTFLAGS pHostFlags,
                                               bool fRenderThreadMode)
{
    LogRelFlowFunc(("uScreenId %d\n", uScreenId));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;

    if (pThis->maFramebuffers[uScreenId].fVBVAEnabled && pThis->maFramebuffers[uScreenId].fRenderThreadMode != fRenderThreadMode)
    {
        LogRel(("enabling different vbva mode"));
#ifdef DEBUG_misha
        AssertMsgFailed(("enabling different vbva mode"));
#endif
        return VERR_INVALID_STATE;
    }

    pThis->maFramebuffers[uScreenId].fVBVAEnabled = true;
    pThis->maFramebuffers[uScreenId].pVBVAHostFlags = pHostFlags;
    pThis->maFramebuffers[uScreenId].fRenderThreadMode = fRenderThreadMode;
    pThis->maFramebuffers[uScreenId].fVBVAForceResize = true;

    vbvaSetMemoryFlagsHGSMI(uScreenId, pThis->mfu32SupportedOrders, pThis->mfVideoAccelVRDP, &pThis->maFramebuffers[uScreenId]);

    return VINF_SUCCESS;
}

DECLCALLBACK(void) Display::i_displayVBVADisable(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId)
{
    LogRelFlowFunc(("uScreenId %d\n", uScreenId));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;

    DISPLAYFBINFO *pFBInfo = &pThis->maFramebuffers[uScreenId];

    bool fRenderThreadMode = pFBInfo->fRenderThreadMode;

    if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
    {
        /* Make sure that the primary screen is visible now.
         * The guest can't use VBVA anymore, so only only the VGA device output works.
         */
        if (pFBInfo->fDisabled)
        {
            pFBInfo->fDisabled = false;
            fireGuestMonitorChangedEvent(pThis->mParent->i_getEventSource(),
                                         GuestMonitorChangedEventType_Enabled,
                                         uScreenId,
                                         pFBInfo->xOrigin, pFBInfo->yOrigin,
                                         pFBInfo->w, pFBInfo->h);
        }
    }

    pFBInfo->fVBVAEnabled = false;
    pFBInfo->fVBVAForceResize = false;
    pFBInfo->fRenderThreadMode = false;

    vbvaSetMemoryFlagsHGSMI(uScreenId, 0, false, pFBInfo);

    pFBInfo->pVBVAHostFlags = NULL;

    if (!fRenderThreadMode && uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
    {
        /* Force full screen update, because VGA device must take control, do resize, etc. */
        pThis->mpDrv->pUpPort->pfnUpdateDisplayAll(pThis->mpDrv->pUpPort, /* fFailOnResize = */ false);
    }
}

DECLCALLBACK(void) Display::i_displayVBVAUpdateBegin(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId)
{
    LogFlowFunc(("uScreenId %d\n", uScreenId));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;
    DISPLAYFBINFO *pFBInfo = &pThis->maFramebuffers[uScreenId];

    if (ASMAtomicReadU32(&pThis->mu32UpdateVBVAFlags) > 0)
    {
        vbvaSetMemoryFlagsAllHGSMI(pThis->mfu32SupportedOrders, pThis->mfVideoAccelVRDP, pThis->maFramebuffers,
                                   pThis->mcMonitors);
        ASMAtomicDecU32(&pThis->mu32UpdateVBVAFlags);
    }
}

DECLCALLBACK(void) Display::i_displayVBVAUpdateProcess(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId,
                                                       const PVBVACMDHDR pCmd, size_t cbCmd)
{
    LogFlowFunc(("uScreenId %d pCmd %p cbCmd %d, @%d,%d %dx%d\n", uScreenId, pCmd, cbCmd, pCmd->x, pCmd->y, pCmd->w, pCmd->h));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;
    DISPLAYFBINFO *pFBInfo = &pThis->maFramebuffers[uScreenId];

    if (pFBInfo->fDefaultFormat)
    {
        /* Make sure that framebuffer contains the same image as the guest VRAM. */
        if (   uScreenId == VBOX_VIDEO_PRIMARY_SCREEN
            && !pFBInfo->fDisabled)
        {
            pDrv->pUpPort->pfnUpdateDisplayRect(pDrv->pUpPort, pCmd->x, pCmd->y, pCmd->w, pCmd->h);
        }
        else if (   !pFBInfo->pSourceBitmap.isNull()
                 && !pFBInfo->fDisabled)
        {
            /* Render VRAM content to the framebuffer. */
            BYTE *pAddress = NULL;
            ULONG ulWidth = 0;
            ULONG ulHeight = 0;
            ULONG ulBitsPerPixel = 0;
            ULONG ulBytesPerLine = 0;
            ULONG ulPixelFormat = 0;

            HRESULT hrc = pFBInfo->pSourceBitmap->QueryBitmapInfo(&pAddress,
                                                                  &ulWidth,
                                                                  &ulHeight,
                                                                  &ulBitsPerPixel,
                                                                  &ulBytesPerLine,
                                                                  &ulPixelFormat);
            if (SUCCEEDED(hrc))
            {
                uint32_t width              = pCmd->w;
                uint32_t height             = pCmd->h;

                const uint8_t *pu8Src       = pFBInfo->pu8FramebufferVRAM;
                int32_t xSrc                = pCmd->x - pFBInfo->xOrigin;
                int32_t ySrc                = pCmd->y - pFBInfo->yOrigin;
                uint32_t u32SrcWidth        = pFBInfo->w;
                uint32_t u32SrcHeight       = pFBInfo->h;
                uint32_t u32SrcLineSize     = pFBInfo->u32LineSize;
                uint32_t u32SrcBitsPerPixel = pFBInfo->u16BitsPerPixel;

                uint8_t *pu8Dst             = pAddress;
                int32_t xDst                = xSrc;
                int32_t yDst                = ySrc;
                uint32_t u32DstWidth        = u32SrcWidth;
                uint32_t u32DstHeight       = u32SrcHeight;
                uint32_t u32DstLineSize     = u32DstWidth * 4;
                uint32_t u32DstBitsPerPixel = 32;

                pDrv->pUpPort->pfnCopyRect(pDrv->pUpPort,
                                           width, height,
                                           pu8Src,
                                           xSrc, ySrc,
                                           u32SrcWidth, u32SrcHeight,
                                           u32SrcLineSize, u32SrcBitsPerPixel,
                                           pu8Dst,
                                           xDst, yDst,
                                           u32DstWidth, u32DstHeight,
                                           u32DstLineSize, u32DstBitsPerPixel);
            }
        }
    }

    VBVACMDHDR hdrSaved = *pCmd;

    VBVACMDHDR *pHdrUnconst = (VBVACMDHDR *)pCmd;

    pHdrUnconst->x -= (int16_t)pFBInfo->xOrigin;
    pHdrUnconst->y -= (int16_t)pFBInfo->yOrigin;

    /* @todo new SendUpdate entry which can get a separate cmd header or coords. */
    pThis->mParent->i_consoleVRDPServer()->SendUpdate(uScreenId, pCmd, (uint32_t)cbCmd);

    *pHdrUnconst = hdrSaved;
}

DECLCALLBACK(void) Display::i_displayVBVAUpdateEnd(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId, int32_t x, int32_t y,
                                                   uint32_t cx, uint32_t cy)
{
    LogFlowFunc(("uScreenId %d %d,%d %dx%d\n", uScreenId, x, y, cx, cy));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;
    DISPLAYFBINFO *pFBInfo = &pThis->maFramebuffers[uScreenId];

    /* @todo handleFramebufferUpdate (uScreenId,
     *                                x - pThis->maFramebuffers[uScreenId].xOrigin,
     *                                y - pThis->maFramebuffers[uScreenId].yOrigin,
     *                                cx, cy);
     */
    pThis->i_handleDisplayUpdate(uScreenId, x - pFBInfo->xOrigin, y - pFBInfo->yOrigin, cx, cy);
}

#ifdef DEBUG_sunlover
static void logVBVAResize(const PVBVAINFOVIEW pView, const PVBVAINFOSCREEN pScreen, const DISPLAYFBINFO *pFBInfo)
{
    LogRel(("displayVBVAResize: [%d] %s\n"
            "    pView->u32ViewIndex     %d\n"
            "    pView->u32ViewOffset    0x%08X\n"
            "    pView->u32ViewSize      0x%08X\n"
            "    pView->u32MaxScreenSize 0x%08X\n"
            "    pScreen->i32OriginX      %d\n"
            "    pScreen->i32OriginY      %d\n"
            "    pScreen->u32StartOffset  0x%08X\n"
            "    pScreen->u32LineSize     0x%08X\n"
            "    pScreen->u32Width        %d\n"
            "    pScreen->u32Height       %d\n"
            "    pScreen->u16BitsPerPixel %d\n"
            "    pScreen->u16Flags        0x%04X\n"
            "    pFBInfo->u32Offset             0x%08X\n"
            "    pFBInfo->u32MaxFramebufferSize 0x%08X\n"
            "    pFBInfo->u32InformationSize    0x%08X\n"
            "    pFBInfo->fDisabled             %d\n"
            "    xOrigin, yOrigin, w, h:        %d,%d %dx%d\n"
            "    pFBInfo->u16BitsPerPixel       %d\n"
            "    pFBInfo->pu8FramebufferVRAM    %p\n"
            "    pFBInfo->u32LineSize           0x%08X\n"
            "    pFBInfo->flags                 0x%04X\n"
            "    pFBInfo->pHostEvents           %p\n"
            "    pFBInfo->fDefaultFormat        %d\n"
            "    pFBInfo->fVBVAEnabled    %d\n"
            "    pFBInfo->fVBVAForceResize %d\n"
            "    pFBInfo->pVBVAHostFlags  %p\n"
            "",
            pScreen->u32ViewIndex,
            (pScreen->u16Flags & VBVA_SCREEN_F_DISABLED)? "DISABLED": "ENABLED",
            pView->u32ViewIndex,
            pView->u32ViewOffset,
            pView->u32ViewSize,
            pView->u32MaxScreenSize,
            pScreen->i32OriginX,
            pScreen->i32OriginY,
            pScreen->u32StartOffset,
            pScreen->u32LineSize,
            pScreen->u32Width,
            pScreen->u32Height,
            pScreen->u16BitsPerPixel,
            pScreen->u16Flags,
            pFBInfo->u32Offset,
            pFBInfo->u32MaxFramebufferSize,
            pFBInfo->u32InformationSize,
            pFBInfo->fDisabled,
            pFBInfo->xOrigin,
            pFBInfo->yOrigin,
            pFBInfo->w,
            pFBInfo->h,
            pFBInfo->u16BitsPerPixel,
            pFBInfo->pu8FramebufferVRAM,
            pFBInfo->u32LineSize,
            pFBInfo->flags,
            pFBInfo->pHostEvents,
            pFBInfo->fDefaultFormat,
            pFBInfo->fVBVAEnabled,
            pFBInfo->fVBVAForceResize,
            pFBInfo->pVBVAHostFlags
          ));
}
#endif /* DEBUG_sunlover */

DECLCALLBACK(int) Display::i_displayVBVAResize(PPDMIDISPLAYCONNECTOR pInterface, const PVBVAINFOVIEW pView,
                                               const PVBVAINFOSCREEN pScreen, void *pvVRAM)
{
    LogRelFlowFunc(("pScreen %p, pvVRAM %p\n", pScreen, pvVRAM));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;

    DISPLAYFBINFO *pFBInfo = &pThis->maFramebuffers[pScreen->u32ViewIndex];

    if (pScreen->u16Flags & VBVA_SCREEN_F_DISABLED)
    {
        pThis->i_notifyCroglResize(pView, pScreen, pvVRAM);

        pFBInfo->fDisabled = true;
        pFBInfo->flags = pScreen->u16Flags;

        /* Ask the framebuffer to resize using a default format. The framebuffer will be black.
         * So if the frontend does not support GuestMonitorChangedEventType_Disabled event,
         * the VM window will be black. */
        uint32_t u32Width = pFBInfo->w ? pFBInfo->w : 640;
        uint32_t u32Height = pFBInfo->h ? pFBInfo->h : 480;
        pThis->i_handleDisplayResize(pScreen->u32ViewIndex, 0, (uint8_t *)NULL, 0,
                                     u32Width, u32Height, pScreen->u16Flags);

        fireGuestMonitorChangedEvent(pThis->mParent->i_getEventSource(),
                                     GuestMonitorChangedEventType_Disabled,
                                     pScreen->u32ViewIndex,
                                     0, 0, 0, 0);
        return VINF_SUCCESS;
    }

    /* If display was disabled or there is no framebuffer, a resize will be required,
     * because the framebuffer was/will be changed.
     */
    bool fResize = pFBInfo->fDisabled || pFBInfo->pFramebuffer.isNull();

    if (pFBInfo->fVBVAForceResize)
    {
        /* VBVA was just enabled. Do the resize. */
        fResize = true;
        pFBInfo->fVBVAForceResize = false;
    }

    /* Check if this is a real resize or a notification about the screen origin.
     * The guest uses this VBVAResize call for both.
     */
    fResize =    fResize
              || pFBInfo->u16BitsPerPixel != pScreen->u16BitsPerPixel
              || pFBInfo->pu8FramebufferVRAM != (uint8_t *)pvVRAM + pScreen->u32StartOffset
              || pFBInfo->u32LineSize != pScreen->u32LineSize
              || pFBInfo->w != pScreen->u32Width
              || pFBInfo->h != pScreen->u32Height;

    bool fNewOrigin =    pFBInfo->xOrigin != pScreen->i32OriginX
                      || pFBInfo->yOrigin != pScreen->i32OriginY;

    if (fNewOrigin || fResize)
        pThis->i_notifyCroglResize(pView, pScreen, pvVRAM);

    if (pFBInfo->fDisabled)
    {
        pFBInfo->fDisabled = false;
        fireGuestMonitorChangedEvent(pThis->mParent->i_getEventSource(),
                                     GuestMonitorChangedEventType_Enabled,
                                     pScreen->u32ViewIndex,
                                     pScreen->i32OriginX, pScreen->i32OriginY,
                                     pScreen->u32Width, pScreen->u32Height);
        /* Continue to update pFBInfo. */
    }

    pFBInfo->u32Offset = pView->u32ViewOffset; /* Not used in HGSMI. */
    pFBInfo->u32MaxFramebufferSize = pView->u32MaxScreenSize; /* Not used in HGSMI. */
    pFBInfo->u32InformationSize = 0; /* Not used in HGSMI. */

    pFBInfo->xOrigin = pScreen->i32OriginX;
    pFBInfo->yOrigin = pScreen->i32OriginY;

    pFBInfo->w = pScreen->u32Width;
    pFBInfo->h = pScreen->u32Height;

    pFBInfo->u16BitsPerPixel = pScreen->u16BitsPerPixel;
    pFBInfo->pu8FramebufferVRAM = (uint8_t *)pvVRAM + pScreen->u32StartOffset;
    pFBInfo->u32LineSize = pScreen->u32LineSize;

    pFBInfo->flags = pScreen->u16Flags;

    if (fNewOrigin)
    {
        fireGuestMonitorChangedEvent(pThis->mParent->i_getEventSource(),
                                     GuestMonitorChangedEventType_NewOrigin,
                                     pScreen->u32ViewIndex,
                                     pScreen->i32OriginX, pScreen->i32OriginY,
                                     0, 0);
    }

    if (!fResize)
    {
        /* No parameters of the framebuffer have actually changed. */
        if (fNewOrigin)
        {
            /* VRDP server still need this notification. */
            LogRelFlowFunc(("Calling VRDP\n"));
            pThis->mParent->i_consoleVRDPServer()->SendResize();
        }
        return VINF_SUCCESS;
    }

    /* Do a regular resize. */
    return pThis->i_handleDisplayResize(pScreen->u32ViewIndex, pScreen->u16BitsPerPixel,
                                        (uint8_t *)pvVRAM + pScreen->u32StartOffset,
                                        pScreen->u32LineSize, pScreen->u32Width, pScreen->u32Height, pScreen->u16Flags);
}

DECLCALLBACK(int) Display::i_displayVBVAMousePointerShape(PPDMIDISPLAYCONNECTOR pInterface, bool fVisible, bool fAlpha,
                                                          uint32_t xHot, uint32_t yHot,
                                                          uint32_t cx, uint32_t cy,
                                                          const void *pvShape)
{
    LogFlowFunc(("\n"));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;

    uint32_t cbShape = 0;
    if (pvShape)
    {
        cbShape = (cx + 7) / 8 * cy; /* size of the AND mask */
        cbShape = ((cbShape + 3) & ~3) + cx * 4 * cy; /* + gap + size of the XOR mask */
    }

    /* Tell the console about it */
    pDrv->pDisplay->mParent->i_onMousePointerShapeChange(fVisible, fAlpha,
                                                         xHot, yHot, cx, cy, (uint8_t *)pvShape, cbShape);

    return VINF_SUCCESS;
}
#endif /* VBOX_WITH_HGSMI */

/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
DECLCALLBACK(void *)  Display::i_drvQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVMAINDISPLAY pDrv = PDMINS_2_DATA(pDrvIns, PDRVMAINDISPLAY);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIDISPLAYCONNECTOR, &pDrv->IConnector);
    return NULL;
}


/**
 * Destruct a display driver instance.
 *
 * @returns VBox status.
 * @param   pDrvIns     The driver instance data.
 */
DECLCALLBACK(void) Display::i_drvDestruct(PPDMDRVINS pDrvIns)
{
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    PDRVMAINDISPLAY pThis = PDMINS_2_DATA(pDrvIns, PDRVMAINDISPLAY);
    LogRelFlowFunc(("iInstance=%d\n", pDrvIns->iInstance));

    pThis->pUpPort->pfnSetRenderVRAM(pThis->pUpPort, false);

    pThis->IConnector.pu8Data    = NULL;
    pThis->IConnector.cbScanline = 0;
    pThis->IConnector.cBits      = 32;
    pThis->IConnector.cx         = 0;
    pThis->IConnector.cy         = 0;

    if (pThis->pDisplay)
    {
        AutoWriteLock displayLock(pThis->pDisplay COMMA_LOCKVAL_SRC_POS);
#ifdef VBOX_WITH_VPX
        pThis->pDisplay->i_VideoCaptureStop();
#endif
#ifdef VBOX_WITH_CRHGSMI
        pThis->pDisplay->i_destructCrHgsmiData();
#endif
        pThis->pDisplay->mpDrv = NULL;
        pThis->pDisplay->mpVMMDev = NULL;
    }
}


/**
 * Construct a display driver instance.
 *
 * @copydoc FNPDMDRVCONSTRUCT
 */
DECLCALLBACK(int) Display::i_drvConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    PDRVMAINDISPLAY pThis = PDMINS_2_DATA(pDrvIns, PDRVMAINDISPLAY);
    LogRelFlowFunc(("iInstance=%d\n", pDrvIns->iInstance));

    /*
     * Validate configuration.
     */
    if (!CFGMR3AreValuesValid(pCfg, "Object\0"))
        return VERR_PDM_DRVINS_UNKNOWN_CFG_VALUES;
    AssertMsgReturn(PDMDrvHlpNoAttach(pDrvIns) == VERR_PDM_NO_ATTACHED_DRIVER,
                    ("Configuration error: Not possible to attach anything to this driver!\n"),
                    VERR_PDM_DRVINS_NO_ATTACH);

    /*
     * Init Interfaces.
     */
    pDrvIns->IBase.pfnQueryInterface           = Display::i_drvQueryInterface;

    pThis->IConnector.pfnResize                = Display::i_displayResizeCallback;
    pThis->IConnector.pfnUpdateRect            = Display::i_displayUpdateCallback;
    pThis->IConnector.pfnRefresh               = Display::i_displayRefreshCallback;
    pThis->IConnector.pfnReset                 = Display::i_displayResetCallback;
    pThis->IConnector.pfnLFBModeChange         = Display::i_displayLFBModeChangeCallback;
    pThis->IConnector.pfnProcessAdapterData    = Display::i_displayProcessAdapterDataCallback;
    pThis->IConnector.pfnProcessDisplayData    = Display::i_displayProcessDisplayDataCallback;
#ifdef VBOX_WITH_VIDEOHWACCEL
    pThis->IConnector.pfnVHWACommandProcess    = Display::i_displayVHWACommandProcess;
#endif
#ifdef VBOX_WITH_CRHGSMI
    pThis->IConnector.pfnCrHgsmiCommandProcess = Display::i_displayCrHgsmiCommandProcess;
    pThis->IConnector.pfnCrHgsmiControlProcess = Display::i_displayCrHgsmiControlProcess;
#endif
#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    pThis->IConnector.pfnCrHgcmCtlSubmit       = Display::i_displayCrHgcmCtlSubmit;
#endif
#ifdef VBOX_WITH_HGSMI
    pThis->IConnector.pfnVBVAEnable            = Display::i_displayVBVAEnable;
    pThis->IConnector.pfnVBVADisable           = Display::i_displayVBVADisable;
    pThis->IConnector.pfnVBVAUpdateBegin       = Display::i_displayVBVAUpdateBegin;
    pThis->IConnector.pfnVBVAUpdateProcess     = Display::i_displayVBVAUpdateProcess;
    pThis->IConnector.pfnVBVAUpdateEnd         = Display::i_displayVBVAUpdateEnd;
    pThis->IConnector.pfnVBVAResize            = Display::i_displayVBVAResize;
    pThis->IConnector.pfnVBVAMousePointerShape = Display::i_displayVBVAMousePointerShape;
#endif

    /*
     * Get the IDisplayPort interface of the above driver/device.
     */
    pThis->pUpPort = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIDISPLAYPORT);
    if (!pThis->pUpPort)
    {
        AssertMsgFailed(("Configuration error: No display port interface above!\n"));
        return VERR_PDM_MISSING_INTERFACE_ABOVE;
    }
#if defined(VBOX_WITH_VIDEOHWACCEL) || defined(VBOX_WITH_CRHGSMI)
    pThis->pVBVACallbacks = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIDISPLAYVBVACALLBACKS);
    if (!pThis->pVBVACallbacks)
    {
        AssertMsgFailed(("Configuration error: No VBVA callback interface above!\n"));
        return VERR_PDM_MISSING_INTERFACE_ABOVE;
    }
#endif
    /*
     * Get the Display object pointer and update the mpDrv member.
     */
    void *pv;
    int rc = CFGMR3QueryPtr(pCfg, "Object", &pv);
    if (RT_FAILURE(rc))
    {
        AssertMsgFailed(("Configuration error: No/bad \"Object\" value! rc=%Rrc\n", rc));
        return rc;
    }
    Display *pDisplay = (Display *)pv;      /** @todo Check this cast! */
    pThis->pDisplay = pDisplay;
    pThis->pDisplay->mpDrv = pThis;

    /* Disable VRAM to a buffer copy initially. */
    pThis->pUpPort->pfnSetRenderVRAM(pThis->pUpPort, false);
    pThis->IConnector.cBits = 32; /* DevVGA does nothing otherwise. */

    /*
     * Start periodic screen refreshes
     */
    pThis->pUpPort->pfnSetRefreshRate(pThis->pUpPort, 20);

#ifdef VBOX_WITH_CRHGSMI
    pDisplay->i_setupCrHgsmiData();
#endif

#ifdef VBOX_WITH_VPX
    ComPtr<IMachine> pMachine = pDisplay->mParent->i_machine();
    BOOL fEnabled = false;
    HRESULT hrc = pMachine->COMGETTER(VideoCaptureEnabled)(&fEnabled);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    if (fEnabled)
    {
        rc = pDisplay->i_VideoCaptureStart();
        fireVideoCaptureChangedEvent(pDisplay->mParent->i_getEventSource());
    }
#endif

    return rc;
}


/**
 * Display driver registration record.
 */
const PDMDRVREG Display::DrvReg =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "MainDisplay",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Main display driver (Main as in the API).",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_DISPLAY,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DRVMAINDISPLAY),
    /* pfnConstruct */
    Display::i_drvConstruct,
    /* pfnDestruct */
    Display::i_drvDestruct,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};

/* vi: set tabstop=4 shiftwidth=4 expandtab: */
