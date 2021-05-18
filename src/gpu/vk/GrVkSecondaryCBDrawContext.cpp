/*
 * Copyright 2019 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/vk/GrVkSecondaryCBDrawContext.h"

#include "include/core/SkDeferredDisplayList.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurfaceCharacterization.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/GrRecordingContext.h"
#include "include/gpu/vk/GrVkTypes.h"
#include "src/core/SkSurfacePriv.h"
#include "src/gpu/GrContextThreadSafeProxyPriv.h"
#include "src/gpu/GrDirectContextPriv.h"
#include "src/gpu/GrSurfaceDrawContext.h"
#include "src/gpu/SkGpuDevice.h"

sk_sp<GrVkSecondaryCBDrawContext> GrVkSecondaryCBDrawContext::Make(GrRecordingContext* rContext,
                                                                   const SkImageInfo& imageInfo,
                                                                   const GrVkDrawableInfo& vkInfo,
                                                                   const SkSurfaceProps* props) {
    if (!rContext) {
        return nullptr;
    }

    if (rContext->backend() != GrBackendApi::kVulkan) {
        return nullptr;
    }

    auto rtc = GrSurfaceDrawContext::MakeFromVulkanSecondaryCB(rContext, imageInfo, vkInfo,
                                                               SkSurfacePropsCopyOrDefault(props));
    SkASSERT(rtc->asSurfaceProxy()->isInstantiated());

    auto device = SkGpuDevice::Make(std::move(rtc), SkGpuDevice::kUninit_InitContents);
    if (!device) {
        return nullptr;
    }

    return sk_sp<GrVkSecondaryCBDrawContext>(new GrVkSecondaryCBDrawContext(std::move(device),
                                                                            props));
}

GrVkSecondaryCBDrawContext::GrVkSecondaryCBDrawContext(sk_sp<SkGpuDevice> device,
                                                       const SkSurfaceProps* props)
    : fDevice(device)
    , fProps(SkSurfacePropsCopyOrDefault(props)) {}

GrVkSecondaryCBDrawContext::~GrVkSecondaryCBDrawContext() {
    SkASSERT(!fDevice);
    SkASSERT(!fCachedCanvas.get());
}

SkCanvas* GrVkSecondaryCBDrawContext::getCanvas() {
    if (!fCachedCanvas) {
        fCachedCanvas = std::make_unique<SkCanvas>(fDevice);
    }
    return fCachedCanvas.get();
}

void GrVkSecondaryCBDrawContext::flush() {
    auto dContext = GrAsDirectContext(fDevice->recordingContext());

    if (dContext) {
        dContext->priv().flushSurface(fDevice->targetProxy());
        dContext->submit();
    }
}

bool GrVkSecondaryCBDrawContext::wait(int numSemaphores,
                                      const GrBackendSemaphore waitSemaphores[],
                                      bool deleteSemaphoresAfterWait) {
    return fDevice->wait(numSemaphores, waitSemaphores, deleteSemaphoresAfterWait);
}

void GrVkSecondaryCBDrawContext::releaseResources() {
    fCachedCanvas.reset();
    fDevice.reset();
}

bool GrVkSecondaryCBDrawContext::characterize(SkSurfaceCharacterization* characterization) const {
    auto direct = fDevice->recordingContext()->asDirectContext();
    if (!direct) {
        return false;
    }

    GrSurfaceProxyView readSurfaceView = fDevice->readSurfaceView();
    GrImageInfo grII = fDevice->grImageInfo();

    size_t maxResourceBytes = direct->getResourceCacheLimit();

    // We current don't support textured GrVkSecondaryCBDrawContexts.
    SkASSERT(!readSurfaceView.asTextureProxy());

    SkColorType ct = GrColorTypeToSkColorType(grII.colorInfo().colorType());
    if (ct == kUnknown_SkColorType) {
        return false;
    }

    SkImageInfo ii = SkImageInfo::Make(grII.width(), grII.height(), ct, kPremul_SkAlphaType,
                                       grII.colorInfo().refColorSpace());

    GrBackendFormat format = readSurfaceView.asRenderTargetProxy()->backendFormat();
    int numSamples = readSurfaceView.asRenderTargetProxy()->numSamples();
    GrProtected isProtected = readSurfaceView.asRenderTargetProxy()->isProtected();

    characterization->set(direct->threadSafeProxy(),
                          maxResourceBytes,
                          ii,
                          format,
                          readSurfaceView.origin(),
                          numSamples,
                          SkSurfaceCharacterization::Textureable(false),
                          SkSurfaceCharacterization::MipMapped(false),
                          SkSurfaceCharacterization::UsesGLFBO0(false),
                          SkSurfaceCharacterization::VkRTSupportsInputAttachment(false),
                          SkSurfaceCharacterization::VulkanSecondaryCBCompatible(true),
                          isProtected,
                          this->props());

    return true;
}

bool GrVkSecondaryCBDrawContext::isCompatible(
        const SkSurfaceCharacterization& characterization) const {

    auto dContext = fDevice->recordingContext()->asDirectContext();
    if (!dContext) {
        return false;
    }

    if (!characterization.isValid()) {
        return false;
    }

    if (!characterization.vulkanSecondaryCBCompatible()) {
        return false;
    }

    // As long as the current state in the context allows for greater or equal resources,
    // we allow the DDL to be replayed.
    // DDL TODO: should we just remove the resource check and ignore the cache limits on playback?
    size_t maxResourceBytes = dContext->getResourceCacheLimit();

    if (characterization.isTextureable()) {
        // We don't support textureable DDL when rendering to a GrVkSecondaryCBDrawContext.
        return false;
    }

    if (characterization.usesGLFBO0()) {
        return false;
    }

    GrSurfaceProxyView readSurfaceView = fDevice->readSurfaceView();
    GrImageInfo grII = fDevice->grImageInfo();

    SkColorType rtColorType = GrColorTypeToSkColorType(grII.colorInfo().colorType());
    if (rtColorType == kUnknown_SkColorType) {
        return false;
    }

    GrBackendFormat format = readSurfaceView.asRenderTargetProxy()->backendFormat();
    int numSamples = readSurfaceView.asRenderTargetProxy()->numSamples();
    GrProtected isProtected = readSurfaceView.asRenderTargetProxy()->isProtected();

    return characterization.contextInfo() &&
           characterization.contextInfo()->priv().matches(dContext) &&
           characterization.cacheMaxResourceBytes() <= maxResourceBytes &&
           characterization.origin() == readSurfaceView.origin() &&
           characterization.backendFormat() == format &&
           characterization.width() == grII.width() &&
           characterization.height() == grII.height() &&
           characterization.colorType() == rtColorType &&
           characterization.sampleCount() == numSamples &&
           SkColorSpace::Equals(characterization.colorSpace(), grII.colorInfo().colorSpace()) &&
           characterization.isProtected() == isProtected &&
           characterization.surfaceProps() == fDevice->surfaceProps();
}

#ifndef SK_DDL_IS_UNIQUE_POINTER
bool GrVkSecondaryCBDrawContext::draw(sk_sp<const SkDeferredDisplayList> ddl) {
#else
bool GrVkSecondaryCBDrawContext::draw(const SkDeferredDisplayList* ddl) {
#endif
    if (!ddl || !this->isCompatible(ddl->characterization())) {
        return false;
    }

    auto direct = fDevice->recordingContext()->asDirectContext();
    if (!direct) {
        return false;
    }

    GrSurfaceProxyView readSurfaceView = fDevice->readSurfaceView();

    direct->priv().createDDLTask(std::move(ddl), readSurfaceView.asRenderTargetProxyRef(), {0, 0});
    return true;
}
