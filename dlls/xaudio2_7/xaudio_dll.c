/*
 * Copyright (c) 2015 Mark Harmstone
 * Copyright (c) 2015 Andrew Eikum for CodeWeavers
 * Copyright (c) 2018 Ethan Lee for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include <stdarg.h>

#define NONAMELESSUNION
#define COBJMACROS

#include "initguid.h"
#include "xaudio_private.h"
#include "xaudio2fx.h"
#if XAUDIO2_VER >= 8
#include "xapofx.h"
#endif

#include "ole2.h"
#include "rpcproxy.h"

#include "wine/debug.h"
#include "wine/heap.h"

WINE_DEFAULT_DEBUG_CHANNEL(xaudio2);

#if XAUDIO2_VER != 0 && defined(__i386__)
/* EVE Online uses an OnVoiceProcessingPassStart callback which corrupts %esi. */
#define IXAudio2VoiceCallback_OnVoiceProcessingPassStart(a, b) call_on_voice_processing_pass_start(a, b)
extern void call_on_voice_processing_pass_start(IXAudio2VoiceCallback *This, UINT32 BytesRequired);
__ASM_GLOBAL_FUNC( call_on_voice_processing_pass_start,
                   "pushl %ebp\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t")
                   __ASM_CFI(".cfi_rel_offset %ebp,0\n\t")
                   "movl %esp,%ebp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register %ebp\n\t")
                   "pushl %esi\n\t"
                  __ASM_CFI(".cfi_rel_offset %esi,-4\n\t")
                   "pushl %edi\n\t"
                  __ASM_CFI(".cfi_rel_offset %edi,-8\n\t")
                   "subl $8,%esp\n\t"
                   "pushl 12(%ebp)\n\t"     /* BytesRequired */
                   "pushl 8(%ebp)\n\t"      /* This */
                   "movl 8(%ebp),%eax\n\t"
                   "movl 0(%eax),%eax\n\t"
                   "call *0(%eax)\n\t"      /* This->lpVtbl->OnVoiceProcessingPassStart */
                   "leal -8(%ebp),%esp\n\t"
                   "popl %edi\n\t"
                   __ASM_CFI(".cfi_same_value %edi\n\t")
                   "popl %esi\n\t"
                   __ASM_CFI(".cfi_same_value %esi\n\t")
                   "popl %ebp\n\t"
                   __ASM_CFI(".cfi_def_cfa %esp,4\n\t")
                   __ASM_CFI(".cfi_same_value %ebp\n\t")
                   "ret" )
#endif

static HINSTANCE instance;

static XA2VoiceImpl *impl_from_IXAudio2Voice(IXAudio2Voice *iface);

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, void *pReserved)
{
    TRACE("(%p, %d, %p)\n", hinstDLL, reason, pReserved);

    switch (reason)
    {
#ifndef _WIN64
    case DLL_WINE_PREATTACH:
        return FALSE;  /* prefer native version */
#endif
    case DLL_PROCESS_ATTACH:
        instance = hinstDLL;
        DisableThreadLibraryCalls( hinstDLL );
        break;
    }
    return TRUE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

HRESULT WINAPI DllRegisterServer(void)
{
    TRACE("\n");
    return __wine_register_resources(instance);
}

HRESULT WINAPI DllUnregisterServer(void)
{
    TRACE("\n");
    return __wine_unregister_resources(instance);
}

/* Effect Wrapping */

static inline XA2XAPOImpl *impl_from_FAPO(FAPO *iface)
{
    return CONTAINING_RECORD(iface, XA2XAPOImpl, FAPO_vtbl);
}

static int32_t FAPOCALL XAPO_AddRef(void *iface)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    return InterlockedIncrement(&This->ref);
}

static int32_t FAPOCALL XAPO_Release(void *iface)
{
    int32_t r;
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    r = InterlockedDecrement(&This->ref);
    if(r == 0){
        IXAPO_Release(This->xapo);
        if(This->xapo_params)
            IXAPOParameters_Release(This->xapo_params);
        heap_free(This);
    }
    return r;
}

static uint32_t FAPOCALL XAPO_GetRegistrationProperties(void *iface,
        FAPORegistrationProperties **ppRegistrationProperties)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    XAPO_REGISTRATION_PROPERTIES *xprops;
    HRESULT hr;

    TRACE("%p\n", This);

    hr = IXAPO_GetRegistrationProperties(This->xapo, &xprops);
    if(FAILED(hr))
        return hr;

    /* TODO: check for version == 20 and use XAPO20_REGISTRATION_PROPERTIES */
    *ppRegistrationProperties = (FAPORegistrationProperties*) xprops;
    return 0;
}

static uint32_t FAPOCALL XAPO_IsInputFormatSupported(void *iface,
        const FAudioWaveFormatEx *pOutputFormat, const FAudioWaveFormatEx *pRequestedInputFormat,
        FAudioWaveFormatEx **ppSupportedInputFormat)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    return IXAPO_IsInputFormatSupported(This->xapo, (const WAVEFORMATEX*)pOutputFormat,
            (const WAVEFORMATEX*)pRequestedInputFormat, (WAVEFORMATEX**)ppSupportedInputFormat);
}

static uint32_t FAPOCALL XAPO_IsOutputFormatSupported(void *iface,
        const FAudioWaveFormatEx *pInputFormat, const FAudioWaveFormatEx *pRequestedOutputFormat,
        FAudioWaveFormatEx **ppSupportedOutputFormat)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    return IXAPO_IsOutputFormatSupported(This->xapo, (const WAVEFORMATEX *)pInputFormat,
            (const WAVEFORMATEX *)pRequestedOutputFormat, (WAVEFORMATEX**)ppSupportedOutputFormat);
}

static uint32_t FAPOCALL XAPO_Initialize(void *iface, const void *pData,
        uint32_t DataByteSize)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    return IXAPO_Initialize(This->xapo, pData, DataByteSize);
}

static void FAPOCALL XAPO_Reset(void *iface)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    IXAPO_Reset(This->xapo);
}

static uint32_t FAPOCALL XAPO_LockForProcess(void *iface,
        uint32_t InputLockedParameterCount,
        const FAPOLockForProcessBufferParameters *pInputLockedParameters,
        uint32_t OutputLockedParameterCount,
        const FAPOLockForProcessBufferParameters *pOutputLockedParameters)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    return IXAPO_LockForProcess(This->xapo,
            InputLockedParameterCount,
            (const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *)pInputLockedParameters,
            OutputLockedParameterCount,
            (const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *)pOutputLockedParameters);
}

static void FAPOCALL XAPO_UnlockForProcess(void *iface)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    IXAPO_UnlockForProcess(This->xapo);
}

static void FAPOCALL XAPO_Process(void *iface,
        uint32_t InputProcessParameterCount,
        const FAPOProcessBufferParameters* pInputProcessParameters,
        uint32_t OutputProcessParameterCount,
        FAPOProcessBufferParameters* pOutputProcessParameters,
        int32_t IsEnabled)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    IXAPO_Process(This->xapo, InputProcessParameterCount,
            (const XAPO_PROCESS_BUFFER_PARAMETERS *)pInputProcessParameters,
            OutputProcessParameterCount,
            (XAPO_PROCESS_BUFFER_PARAMETERS *)pOutputProcessParameters,
            IsEnabled);
}

static uint32_t FAPOCALL XAPO_CalcInputFrames(void *iface,
        uint32_t OutputFrameCount)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    return IXAPO_CalcInputFrames(This->xapo, OutputFrameCount);
}

static uint32_t FAPOCALL XAPO_CalcOutputFrames(void *iface,
        uint32_t InputFrameCount)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    return IXAPO_CalcOutputFrames(This->xapo, InputFrameCount);
}

static void FAPOCALL XAPO_SetParameters(void *iface,
        const void *pParameters, uint32_t ParametersByteSize)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    if(This->xapo_params)
        IXAPOParameters_SetParameters(This->xapo_params, pParameters, ParametersByteSize);
}

static void FAPOCALL XAPO_GetParameters(void *iface,
        void *pParameters, uint32_t ParametersByteSize)
{
    XA2XAPOImpl *This = impl_from_FAPO(iface);
    TRACE("%p\n", This);
    if(This->xapo_params)
        IXAPOParameters_GetParameters(This->xapo_params, pParameters, ParametersByteSize);
    else
        memset(pParameters, 0, ParametersByteSize);
}

static const FAPO FAPO_Vtbl = {
    XAPO_AddRef,
    XAPO_Release,
    XAPO_GetRegistrationProperties,
    XAPO_IsInputFormatSupported,
    XAPO_IsOutputFormatSupported,
    XAPO_Initialize,
    XAPO_Reset,
    XAPO_LockForProcess,
    XAPO_UnlockForProcess,
    XAPO_Process,
    XAPO_CalcInputFrames,
    XAPO_CalcOutputFrames,
    XAPO_SetParameters,
    XAPO_GetParameters,
};

static XA2XAPOImpl *wrap_xapo(IUnknown *unk)
{
    XA2XAPOImpl *ret;
    IXAPO *xapo;
    IXAPOParameters *xapo_params;
    HRESULT hr;

#if XAUDIO2_VER <= 7
    hr = IUnknown_QueryInterface(unk, &IID_IXAPO27, (void**)&xapo);
#else
    hr = IUnknown_QueryInterface(unk, &IID_IXAPO, (void**)&xapo);
#endif
    if(FAILED(hr)){
        WARN("XAPO doesn't support IXAPO? %p\n", unk);
        return NULL;
    }

#if XAUDIO2_VER <= 7
    hr = IUnknown_QueryInterface(unk, &IID_IXAPO27Parameters, (void**)&xapo_params);
#else
    hr = IUnknown_QueryInterface(unk, &IID_IXAPOParameters, (void**)&xapo_params);
#endif
    if(FAILED(hr)){
        TRACE("XAPO doesn't support IXAPOParameters %p\n", unk);
        xapo_params = NULL;
    }

    ret = heap_alloc(sizeof(*ret));

    ret->xapo = xapo;
    ret->xapo_params = xapo_params;
    ret->FAPO_vtbl = FAPO_Vtbl;
    ret->ref = 1;

    TRACE("wrapped IXAPO %p with %p\n", xapo, ret);

    return ret;
}

FAudioEffectChain *wrap_effect_chain(const XAUDIO2_EFFECT_CHAIN *pEffectChain)
{
    FAudioEffectChain *ret;
    int i;

    if(!pEffectChain)
        return NULL;

    ret = heap_alloc(sizeof(*ret) + sizeof(FAudioEffectDescriptor) * pEffectChain->EffectCount);

    ret->EffectCount = pEffectChain->EffectCount;
    ret->pEffectDescriptors = (void*)(ret + 1);

    for(i = 0; i < ret->EffectCount; ++i){
        ret->pEffectDescriptors[i].pEffect = &wrap_xapo(pEffectChain->pEffectDescriptors[i].pEffect)->FAPO_vtbl;
        ret->pEffectDescriptors[i].InitialState = pEffectChain->pEffectDescriptors[i].InitialState;
        ret->pEffectDescriptors[i].OutputChannels = pEffectChain->pEffectDescriptors[i].OutputChannels;
    }

    return ret;
}

static void free_effect_chain(FAudioEffectChain *chain)
{
    int i;
    if(!chain)
        return;
    for(i = 0; i < chain->EffectCount; ++i)
        XAPO_Release(chain->pEffectDescriptors[i].pEffect);
    heap_free(chain);
}

/* Send Wrapping */

static FAudioVoiceSends *wrap_voice_sends(const XAUDIO2_VOICE_SENDS *sends)
{
    FAudioVoiceSends *ret;
    int i;

    if(!sends)
        return NULL;

    ret = heap_alloc(sizeof(*ret) + sends->SendCount * sizeof(FAudioSendDescriptor));
    ret->SendCount = sends->SendCount;
    ret->pSends = (FAudioSendDescriptor*)(ret + 1);
    for(i = 0; i < sends->SendCount; ++i){
        XA2VoiceImpl *voice = impl_from_IXAudio2Voice(sends->pSends[i].pOutputVoice);
        ret->pSends[i].pOutputVoice = voice->faudio_voice;
        ret->pSends[i].Flags = sends->pSends[i].Flags;
    }
    return ret;
}

static void free_voice_sends(FAudioVoiceSends *sends)
{
    if(!sends)
        return;
    heap_free(sends);
}

/* Voice Callbacks */

static inline XA2VoiceImpl *impl_from_FAudioVoiceCallback(FAudioVoiceCallback *iface)
{
    return CONTAINING_RECORD(iface, XA2VoiceImpl, FAudioVoiceCallback_vtbl);
}

static void FAUDIOCALL XA2VCB_OnVoiceProcessingPassStart(FAudioVoiceCallback *iface,
        UINT32 BytesRequired)
{
    XA2VoiceImpl *This = impl_from_FAudioVoiceCallback(iface);
    TRACE("%p\n", This);
    if(This->cb)
#if XAUDIO2_VER == 0
        IXAudio20VoiceCallback_OnVoiceProcessingPassStart((IXAudio20VoiceCallback*)This->cb);
#else
        IXAudio2VoiceCallback_OnVoiceProcessingPassStart(This->cb, BytesRequired);
#endif
}

static void FAUDIOCALL XA2VCB_OnVoiceProcessingPassEnd(FAudioVoiceCallback *iface)
{
    XA2VoiceImpl *This = impl_from_FAudioVoiceCallback(iface);
    TRACE("%p\n", This);
    if(This->cb)
        IXAudio2VoiceCallback_OnVoiceProcessingPassEnd(This->cb);
}

static void FAUDIOCALL XA2VCB_OnStreamEnd(FAudioVoiceCallback *iface)
{
    XA2VoiceImpl *This = impl_from_FAudioVoiceCallback(iface);
    TRACE("%p\n", This);
    if(This->cb)
        IXAudio2VoiceCallback_OnStreamEnd(This->cb);
}

static void FAUDIOCALL XA2VCB_OnBufferStart(FAudioVoiceCallback *iface,
        void *pBufferContext)
{
    XA2VoiceImpl *This = impl_from_FAudioVoiceCallback(iface);
    TRACE("%p\n", This);
    if(This->cb)
        IXAudio2VoiceCallback_OnBufferStart(This->cb, pBufferContext);
}

static void FAUDIOCALL XA2VCB_OnBufferEnd(FAudioVoiceCallback *iface,
        void *pBufferContext)
{
    XA2VoiceImpl *This = impl_from_FAudioVoiceCallback(iface);
    TRACE("%p\n", This);
    if(This->cb)
        IXAudio2VoiceCallback_OnBufferEnd(This->cb, pBufferContext);
}

static void FAUDIOCALL XA2VCB_OnLoopEnd(FAudioVoiceCallback *iface,
        void *pBufferContext)
{
    XA2VoiceImpl *This = impl_from_FAudioVoiceCallback(iface);
    TRACE("%p\n", This);
    if(This->cb)
        IXAudio2VoiceCallback_OnLoopEnd(This->cb, pBufferContext);
}

static void FAUDIOCALL XA2VCB_OnVoiceError(FAudioVoiceCallback *iface,
        void *pBufferContext, unsigned int Error)
{
    XA2VoiceImpl *This = impl_from_FAudioVoiceCallback(iface);
    TRACE("%p\n", This);
    if(This->cb)
        IXAudio2VoiceCallback_OnVoiceError(This->cb, pBufferContext, (HRESULT)Error);
}

static const FAudioVoiceCallback FAudioVoiceCallback_Vtbl = {
    XA2VCB_OnBufferEnd,
    XA2VCB_OnBufferStart,
    XA2VCB_OnLoopEnd,
    XA2VCB_OnStreamEnd,
    XA2VCB_OnVoiceError,
    XA2VCB_OnVoiceProcessingPassEnd,
    XA2VCB_OnVoiceProcessingPassStart
};

/* Engine Callbacks */

static inline IXAudio2Impl *impl_from_FAudioEngineCallback(FAudioEngineCallback *iface)
{
    return CONTAINING_RECORD(iface, IXAudio2Impl, FAudioEngineCallback_vtbl);
}

static void FAUDIOCALL XA2ECB_OnProcessingPassStart(FAudioEngineCallback *iface)
{
    IXAudio2Impl *This = impl_from_FAudioEngineCallback(iface);
    int i;
    TRACE("%p\n", This);
    for(i = 0; i < This->ncbs && This->cbs[i]; ++i)
        IXAudio2EngineCallback_OnProcessingPassStart(This->cbs[i]);
}

static void FAUDIOCALL XA2ECB_OnProcessingPassEnd(FAudioEngineCallback *iface)
{
    IXAudio2Impl *This = impl_from_FAudioEngineCallback(iface);
    int i;
    TRACE("%p\n", This);
    for(i = 0; i < This->ncbs && This->cbs[i]; ++i)
        IXAudio2EngineCallback_OnProcessingPassEnd(This->cbs[i]);
}

static void FAUDIOCALL XA2ECB_OnCriticalError(FAudioEngineCallback *iface,
        uint32_t error)
{
    IXAudio2Impl *This = impl_from_FAudioEngineCallback(iface);
    int i;
    TRACE("%p\n", This);
    for(i = 0; i < This->ncbs && This->cbs[i]; ++i)
        IXAudio2EngineCallback_OnCriticalError(This->cbs[i], error);
}

static const FAudioEngineCallback FAudioEngineCallback_Vtbl = {
    XA2ECB_OnCriticalError,
    XA2ECB_OnProcessingPassEnd,
    XA2ECB_OnProcessingPassStart
};

/* Common Voice Functions */

static inline void destroy_voice(XA2VoiceImpl *This)
{
    FAudioVoice_DestroyVoice(This->faudio_voice);
    free_effect_chain(This->effect_chain);
    This->effect_chain = NULL;
    This->in_use = FALSE;
}

/* Source Voices */

static inline XA2VoiceImpl *impl_from_IXAudio2SourceVoice(IXAudio2SourceVoice *iface)
{
    return CONTAINING_RECORD(iface, XA2VoiceImpl, IXAudio2SourceVoice_iface);
}

static void WINAPI XA2SRC_GetVoiceDetails(IXAudio2SourceVoice *iface,
        XAUDIO2_VOICE_DETAILS *pVoiceDetails)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %p\n", This, pVoiceDetails);
    FAudioVoice_GetVoiceDetails(This->faudio_voice, (FAudioVoiceDetails *)pVoiceDetails);
}

static HRESULT WINAPI XA2SRC_SetOutputVoices(IXAudio2SourceVoice *iface,
        const XAUDIO2_VOICE_SENDS *pSendList)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    FAudioVoiceSends *faudio_sends;
    HRESULT hr;

    TRACE("%p, %p\n", This, pSendList);

    faudio_sends = wrap_voice_sends(pSendList);

    hr = FAudioVoice_SetOutputVoices(This->faudio_voice, faudio_sends);

    free_voice_sends(faudio_sends);

    return hr;
}

static HRESULT WINAPI XA2SRC_SetEffectChain(IXAudio2SourceVoice *iface,
        const XAUDIO2_EFFECT_CHAIN *pEffectChain)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    HRESULT hr;

    TRACE("%p, %p\n", This, pEffectChain);

    free_effect_chain(This->effect_chain);
    This->effect_chain = wrap_effect_chain(pEffectChain);

    hr = FAudioVoice_SetEffectChain(This->faudio_voice, This->effect_chain);

    return hr;
}

static HRESULT WINAPI XA2SRC_EnableEffect(IXAudio2SourceVoice *iface, UINT32 EffectIndex,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %u, 0x%x\n", This, EffectIndex, OperationSet);
    return FAudioVoice_EnableEffect(This->faudio_voice, EffectIndex, OperationSet);
}

static HRESULT WINAPI XA2SRC_DisableEffect(IXAudio2SourceVoice *iface, UINT32 EffectIndex,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %u, 0x%x\n", This, EffectIndex, OperationSet);
    return FAudioVoice_DisableEffect(This->faudio_voice, EffectIndex, OperationSet);
}

static void WINAPI XA2SRC_GetEffectState(IXAudio2SourceVoice *iface, UINT32 EffectIndex,
        BOOL *pEnabled)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %u, %p\n", This, EffectIndex, pEnabled);
    FAudioVoice_GetEffectState(This->faudio_voice, EffectIndex, (int32_t*)pEnabled);
}

static HRESULT WINAPI XA2SRC_SetEffectParameters(IXAudio2SourceVoice *iface,
        UINT32 EffectIndex, const void *pParameters, UINT32 ParametersByteSize,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %u, %p, 0x%x, 0x%x\n", This, EffectIndex, pParameters,
            ParametersByteSize, OperationSet);
    return FAudioVoice_SetEffectParameters(This->faudio_voice, EffectIndex,
            pParameters, ParametersByteSize, OperationSet);
}

static HRESULT WINAPI XA2SRC_GetEffectParameters(IXAudio2SourceVoice *iface,
        UINT32 EffectIndex, void *pParameters, UINT32 ParametersByteSize)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %u, %p, 0x%x\n", This, EffectIndex, pParameters,
            ParametersByteSize);
    return FAudioVoice_GetEffectParameters(This->faudio_voice, EffectIndex,
            pParameters, ParametersByteSize);
}

static HRESULT WINAPI XA2SRC_SetFilterParameters(IXAudio2SourceVoice *iface,
        const XAUDIO2_FILTER_PARAMETERS *pParameters, UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %p, 0x%x\n", This, pParameters, OperationSet);
    return FAudioVoice_SetFilterParameters(This->faudio_voice,
            (const FAudioFilterParameters *)pParameters, OperationSet);
}

static void WINAPI XA2SRC_GetFilterParameters(IXAudio2SourceVoice *iface,
        XAUDIO2_FILTER_PARAMETERS *pParameters)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %p\n", This, pParameters);
    FAudioVoice_GetFilterParameters(This->faudio_voice, (FAudioFilterParameters *)pParameters);
}

static HRESULT WINAPI XA2SRC_SetOutputFilterParameters(IXAudio2SourceVoice *iface,
        IXAudio2Voice *pDestinationVoice,
        const XAUDIO2_FILTER_PARAMETERS *pParameters, UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %p, 0x%x\n", This, pDestinationVoice, pParameters, OperationSet);

    return FAudioVoice_SetOutputFilterParameters(This->faudio_voice,
            dst ? dst->faudio_voice : NULL, (const FAudioFilterParameters *)pParameters, OperationSet);
}

static void WINAPI XA2SRC_GetOutputFilterParameters(IXAudio2SourceVoice *iface,
        IXAudio2Voice *pDestinationVoice,
        XAUDIO2_FILTER_PARAMETERS *pParameters)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %p\n", This, pDestinationVoice, pParameters);

    FAudioVoice_GetOutputFilterParameters(This->faudio_voice,
            dst ? dst->faudio_voice : NULL, (FAudioFilterParameters *)pParameters);
}

static HRESULT WINAPI XA2SRC_SetVolume(IXAudio2SourceVoice *iface, float Volume,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %f, 0x%x\n", This, Volume, OperationSet);
    return FAudioVoice_SetVolume(This->faudio_voice, Volume, OperationSet);
}

static void WINAPI XA2SRC_GetVolume(IXAudio2SourceVoice *iface, float *pVolume)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %p\n", This, pVolume);
    return FAudioVoice_GetVolume(This->faudio_voice, pVolume);
}

static HRESULT WINAPI XA2SRC_SetChannelVolumes(IXAudio2SourceVoice *iface, UINT32 Channels,
        const float *pVolumes, UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %u, %p, 0x%x\n", This, Channels, pVolumes, OperationSet);
    return FAudioVoice_SetChannelVolumes(This->faudio_voice, Channels,
            pVolumes, OperationSet);
}

static void WINAPI XA2SRC_GetChannelVolumes(IXAudio2SourceVoice *iface, UINT32 Channels,
        float *pVolumes)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    TRACE("%p, %u, %p\n", This, Channels, pVolumes);
    return FAudioVoice_GetChannelVolumes(This->faudio_voice, Channels,
            pVolumes);
}

static HRESULT WINAPI XA2SRC_SetOutputMatrix(IXAudio2SourceVoice *iface,
        IXAudio2Voice *pDestinationVoice, UINT32 SourceChannels,
        UINT32 DestinationChannels, const float *pLevelMatrix,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %u, %u, %p, 0x%x\n", This, pDestinationVoice,
            SourceChannels, DestinationChannels, pLevelMatrix, OperationSet);

    return FAudioVoice_SetOutputMatrix(This->faudio_voice, dst ? dst->faudio_voice : NULL,
            SourceChannels, DestinationChannels, pLevelMatrix, OperationSet);
}

static void WINAPI XA2SRC_GetOutputMatrix(IXAudio2SourceVoice *iface,
        IXAudio2Voice *pDestinationVoice, UINT32 SourceChannels,
        UINT32 DestinationChannels, float *pLevelMatrix)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %u, %u, %p\n", This, pDestinationVoice,
            SourceChannels, DestinationChannels, pLevelMatrix);

    FAudioVoice_GetOutputMatrix(This->faudio_voice, dst ? dst->faudio_voice : NULL,
            SourceChannels, DestinationChannels, pLevelMatrix);
}

static void WINAPI XA2SRC_DestroyVoice(IXAudio2SourceVoice *iface)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);

    TRACE("%p\n", This);

    EnterCriticalSection(&This->lock);

    destroy_voice(This);

    LeaveCriticalSection(&This->lock);
}

static HRESULT WINAPI XA2SRC_Start(IXAudio2SourceVoice *iface, UINT32 Flags,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);

    TRACE("%p, 0x%x, 0x%x\n", This, Flags, OperationSet);

    return FAudioSourceVoice_Start(This->faudio_voice, Flags, OperationSet);
}

static HRESULT WINAPI XA2SRC_Stop(IXAudio2SourceVoice *iface, UINT32 Flags,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);

    TRACE("%p, 0x%x, 0x%x\n", This, Flags, OperationSet);

    return FAudioSourceVoice_Stop(This->faudio_voice, Flags, OperationSet);
}

static HRESULT WINAPI XA2SRC_SubmitSourceBuffer(IXAudio2SourceVoice *iface,
        const XAUDIO2_BUFFER *pBuffer, const XAUDIO2_BUFFER_WMA *pBufferWMA)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);

    TRACE("%p, %p, %p\n", This, pBuffer, pBufferWMA);

    return FAudioSourceVoice_SubmitSourceBuffer(This->faudio_voice, (FAudioBuffer*)pBuffer, (FAudioBufferWMA*)pBufferWMA);
}

static HRESULT WINAPI XA2SRC_FlushSourceBuffers(IXAudio2SourceVoice *iface)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);

    TRACE("%p\n", This);

    return FAudioSourceVoice_FlushSourceBuffers(This->faudio_voice);
}

static HRESULT WINAPI XA2SRC_Discontinuity(IXAudio2SourceVoice *iface)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);

    TRACE("%p\n", This);

    return FAudioSourceVoice_Discontinuity(This->faudio_voice);
}

static HRESULT WINAPI XA2SRC_ExitLoop(IXAudio2SourceVoice *iface, UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);

    TRACE("%p, 0x%x\n", This, OperationSet);

    return FAudioSourceVoice_ExitLoop(This->faudio_voice, OperationSet);
}

static void WINAPI XA2SRC_GetState(IXAudio2SourceVoice *iface,
        XAUDIO2_VOICE_STATE *pVoiceState, UINT32 Flags)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);

    TRACE("%p, %p, 0x%x\n", This, pVoiceState, Flags);

    return FAudioSourceVoice_GetState(This->faudio_voice, (FAudioVoiceState*)pVoiceState, Flags);
}

static HRESULT WINAPI XA2SRC_SetFrequencyRatio(IXAudio2SourceVoice *iface,
        float Ratio, UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);

    TRACE("%p, %f, 0x%x\n", This, Ratio, OperationSet);

    return FAudioSourceVoice_SetFrequencyRatio(This->faudio_voice, Ratio, OperationSet);
}

static void WINAPI XA2SRC_GetFrequencyRatio(IXAudio2SourceVoice *iface, float *pRatio)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);

    TRACE("%p, %p\n", This, pRatio);

    return FAudioSourceVoice_GetFrequencyRatio(This->faudio_voice, pRatio);
}

static HRESULT WINAPI XA2SRC_SetSourceSampleRate(
    IXAudio2SourceVoice *iface,
    UINT32 NewSourceSampleRate)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SourceVoice(iface);

    TRACE("%p, %u\n", This, NewSourceSampleRate);

    return FAudioSourceVoice_SetSourceSampleRate(This->faudio_voice, NewSourceSampleRate);
}

static const IXAudio2SourceVoiceVtbl XAudio2SourceVoice_Vtbl = {
    XA2SRC_GetVoiceDetails,
    XA2SRC_SetOutputVoices,
    XA2SRC_SetEffectChain,
    XA2SRC_EnableEffect,
    XA2SRC_DisableEffect,
    XA2SRC_GetEffectState,
    XA2SRC_SetEffectParameters,
    XA2SRC_GetEffectParameters,
    XA2SRC_SetFilterParameters,
    XA2SRC_GetFilterParameters,
    XA2SRC_SetOutputFilterParameters,
    XA2SRC_GetOutputFilterParameters,
    XA2SRC_SetVolume,
    XA2SRC_GetVolume,
    XA2SRC_SetChannelVolumes,
    XA2SRC_GetChannelVolumes,
    XA2SRC_SetOutputMatrix,
    XA2SRC_GetOutputMatrix,
    XA2SRC_DestroyVoice,
    XA2SRC_Start,
    XA2SRC_Stop,
    XA2SRC_SubmitSourceBuffer,
    XA2SRC_FlushSourceBuffers,
    XA2SRC_Discontinuity,
    XA2SRC_ExitLoop,
    XA2SRC_GetState,
    XA2SRC_SetFrequencyRatio,
    XA2SRC_GetFrequencyRatio,
    XA2SRC_SetSourceSampleRate
};

/* Submix Voices */

static inline XA2VoiceImpl *impl_from_IXAudio2SubmixVoice(IXAudio2SubmixVoice *iface)
{
    return CONTAINING_RECORD(iface, XA2VoiceImpl, IXAudio2SubmixVoice_iface);
}

static void WINAPI XA2SUB_GetVoiceDetails(IXAudio2SubmixVoice *iface,
        XAUDIO2_VOICE_DETAILS *pVoiceDetails)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %p\n", This, pVoiceDetails);
    FAudioVoice_GetVoiceDetails(This->faudio_voice, (FAudioVoiceDetails *)pVoiceDetails);
}

static HRESULT WINAPI XA2SUB_SetOutputVoices(IXAudio2SubmixVoice *iface,
        const XAUDIO2_VOICE_SENDS *pSendList)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    FAudioVoiceSends *faudio_sends;
    HRESULT hr;

    TRACE("%p, %p\n", This, pSendList);

    faudio_sends = wrap_voice_sends(pSendList);

    hr = FAudioVoice_SetOutputVoices(This->faudio_voice, faudio_sends);

    free_voice_sends(faudio_sends);

    return hr;
}

static HRESULT WINAPI XA2SUB_SetEffectChain(IXAudio2SubmixVoice *iface,
        const XAUDIO2_EFFECT_CHAIN *pEffectChain)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    HRESULT hr;

    TRACE("%p, %p\n", This, pEffectChain);

    free_effect_chain(This->effect_chain);
    This->effect_chain = wrap_effect_chain(pEffectChain);

    hr = FAudioVoice_SetEffectChain(This->faudio_voice, This->effect_chain);

    return hr;
}

static HRESULT WINAPI XA2SUB_EnableEffect(IXAudio2SubmixVoice *iface, UINT32 EffectIndex,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %u, 0x%x\n", This, EffectIndex, OperationSet);
    return FAudioVoice_EnableEffect(This->faudio_voice, EffectIndex, OperationSet);
}

static HRESULT WINAPI XA2SUB_DisableEffect(IXAudio2SubmixVoice *iface, UINT32 EffectIndex,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %u, 0x%x\n", This, EffectIndex, OperationSet);
    return FAudioVoice_DisableEffect(This->faudio_voice, EffectIndex, OperationSet);
}

static void WINAPI XA2SUB_GetEffectState(IXAudio2SubmixVoice *iface, UINT32 EffectIndex,
        BOOL *pEnabled)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %u, %p\n", This, EffectIndex, pEnabled);
    FAudioVoice_GetEffectState(This->faudio_voice, EffectIndex, (int32_t*)pEnabled);
}

static HRESULT WINAPI XA2SUB_SetEffectParameters(IXAudio2SubmixVoice *iface,
        UINT32 EffectIndex, const void *pParameters, UINT32 ParametersByteSize,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %u, %p, 0x%x, 0x%x\n", This, EffectIndex, pParameters,
            ParametersByteSize, OperationSet);
    return FAudioVoice_SetEffectParameters(This->faudio_voice, EffectIndex,
            pParameters, ParametersByteSize, OperationSet);
}

static HRESULT WINAPI XA2SUB_GetEffectParameters(IXAudio2SubmixVoice *iface,
        UINT32 EffectIndex, void *pParameters, UINT32 ParametersByteSize)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %u, %p, 0x%x\n", This, EffectIndex, pParameters,
            ParametersByteSize);
    return FAudioVoice_GetEffectParameters(This->faudio_voice, EffectIndex,
            pParameters, ParametersByteSize);
}

static HRESULT WINAPI XA2SUB_SetFilterParameters(IXAudio2SubmixVoice *iface,
        const XAUDIO2_FILTER_PARAMETERS *pParameters, UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %p, 0x%x\n", This, pParameters, OperationSet);
    return FAudioVoice_SetFilterParameters(This->faudio_voice, (const FAudioFilterParameters *)pParameters,
            OperationSet);
}

static void WINAPI XA2SUB_GetFilterParameters(IXAudio2SubmixVoice *iface,
        XAUDIO2_FILTER_PARAMETERS *pParameters)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %p\n", This, pParameters);
    FAudioVoice_GetFilterParameters(This->faudio_voice, (FAudioFilterParameters *)pParameters);
}

static HRESULT WINAPI XA2SUB_SetOutputFilterParameters(IXAudio2SubmixVoice *iface,
        IXAudio2Voice *pDestinationVoice,
        const XAUDIO2_FILTER_PARAMETERS *pParameters, UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %p, 0x%x\n", This, pDestinationVoice, pParameters, OperationSet);

    return FAudioVoice_SetOutputFilterParameters(This->faudio_voice,
            dst ? dst->faudio_voice : NULL, (const FAudioFilterParameters *)pParameters, OperationSet);
}

static void WINAPI XA2SUB_GetOutputFilterParameters(IXAudio2SubmixVoice *iface,
        IXAudio2Voice *pDestinationVoice,
        XAUDIO2_FILTER_PARAMETERS *pParameters)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %p\n", This, pDestinationVoice, pParameters);

    FAudioVoice_GetOutputFilterParameters(This->faudio_voice,
            dst ? dst->faudio_voice : NULL, (FAudioFilterParameters *)pParameters);
}

static HRESULT WINAPI XA2SUB_SetVolume(IXAudio2SubmixVoice *iface, float Volume,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %f, 0x%x\n", This, Volume, OperationSet);
    return FAudioVoice_SetVolume(This->faudio_voice, Volume, OperationSet);
}

static void WINAPI XA2SUB_GetVolume(IXAudio2SubmixVoice *iface, float *pVolume)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %p\n", This, pVolume);
    return FAudioVoice_GetVolume(This->faudio_voice, pVolume);
}

static HRESULT WINAPI XA2SUB_SetChannelVolumes(IXAudio2SubmixVoice *iface, UINT32 Channels,
        const float *pVolumes, UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %u, %p, 0x%x\n", This, Channels, pVolumes, OperationSet);
    return FAudioVoice_SetChannelVolumes(This->faudio_voice, Channels,
            pVolumes, OperationSet);
}

static void WINAPI XA2SUB_GetChannelVolumes(IXAudio2SubmixVoice *iface, UINT32 Channels,
        float *pVolumes)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    TRACE("%p, %u, %p\n", This, Channels, pVolumes);
    return FAudioVoice_GetChannelVolumes(This->faudio_voice, Channels,
            pVolumes);
}

static HRESULT WINAPI XA2SUB_SetOutputMatrix(IXAudio2SubmixVoice *iface,
        IXAudio2Voice *pDestinationVoice, UINT32 SourceChannels,
        UINT32 DestinationChannels, const float *pLevelMatrix,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %u, %u, %p, 0x%x\n", This, pDestinationVoice,
            SourceChannels, DestinationChannels, pLevelMatrix, OperationSet);

    return FAudioVoice_SetOutputMatrix(This->faudio_voice, dst ? dst->faudio_voice : NULL,
            SourceChannels, DestinationChannels, pLevelMatrix, OperationSet);
}

static void WINAPI XA2SUB_GetOutputMatrix(IXAudio2SubmixVoice *iface,
        IXAudio2Voice *pDestinationVoice, UINT32 SourceChannels,
        UINT32 DestinationChannels, float *pLevelMatrix)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %u, %u, %p\n", This, pDestinationVoice,
            SourceChannels, DestinationChannels, pLevelMatrix);

    FAudioVoice_GetOutputMatrix(This->faudio_voice, dst ? dst->faudio_voice : NULL,
            SourceChannels, DestinationChannels, pLevelMatrix);
}

static void WINAPI XA2SUB_DestroyVoice(IXAudio2SubmixVoice *iface)
{
    XA2VoiceImpl *This = impl_from_IXAudio2SubmixVoice(iface);

    TRACE("%p\n", This);

    EnterCriticalSection(&This->lock);

    destroy_voice(This);

    LeaveCriticalSection(&This->lock);
}

static const struct IXAudio2SubmixVoiceVtbl XAudio2SubmixVoice_Vtbl = {
    XA2SUB_GetVoiceDetails,
    XA2SUB_SetOutputVoices,
    XA2SUB_SetEffectChain,
    XA2SUB_EnableEffect,
    XA2SUB_DisableEffect,
    XA2SUB_GetEffectState,
    XA2SUB_SetEffectParameters,
    XA2SUB_GetEffectParameters,
    XA2SUB_SetFilterParameters,
    XA2SUB_GetFilterParameters,
    XA2SUB_SetOutputFilterParameters,
    XA2SUB_GetOutputFilterParameters,
    XA2SUB_SetVolume,
    XA2SUB_GetVolume,
    XA2SUB_SetChannelVolumes,
    XA2SUB_GetChannelVolumes,
    XA2SUB_SetOutputMatrix,
    XA2SUB_GetOutputMatrix,
    XA2SUB_DestroyVoice
};

/* Mastering Voices */

static inline XA2VoiceImpl *impl_from_IXAudio2MasteringVoice(IXAudio2MasteringVoice *iface)
{
    return CONTAINING_RECORD(iface, XA2VoiceImpl, IXAudio2MasteringVoice_iface);
}

static void WINAPI XA2M_GetVoiceDetails(IXAudio2MasteringVoice *iface,
        XAUDIO2_VOICE_DETAILS *pVoiceDetails)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %p\n", This, pVoiceDetails);
    FAudioVoice_GetVoiceDetails(This->faudio_voice, (FAudioVoiceDetails *)pVoiceDetails);
}

static HRESULT WINAPI XA2M_SetOutputVoices(IXAudio2MasteringVoice *iface,
        const XAUDIO2_VOICE_SENDS *pSendList)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    FAudioVoiceSends *faudio_sends;
    HRESULT hr;

    TRACE("%p, %p\n", This, pSendList);

    faudio_sends = wrap_voice_sends(pSendList);

    hr = FAudioVoice_SetOutputVoices(This->faudio_voice, faudio_sends);

    free_voice_sends(faudio_sends);

    return hr;
}

static HRESULT WINAPI XA2M_SetEffectChain(IXAudio2MasteringVoice *iface,
        const XAUDIO2_EFFECT_CHAIN *pEffectChain)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    HRESULT hr;

    TRACE("%p, %p\n", This, pEffectChain);

    free_effect_chain(This->effect_chain);
    This->effect_chain = wrap_effect_chain(pEffectChain);

    hr = FAudioVoice_SetEffectChain(This->faudio_voice, This->effect_chain);

    return hr;
}

static HRESULT WINAPI XA2M_EnableEffect(IXAudio2MasteringVoice *iface, UINT32 EffectIndex,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %u, 0x%x\n", This, EffectIndex, OperationSet);
    return FAudioVoice_EnableEffect(This->faudio_voice, EffectIndex, OperationSet);
}

static HRESULT WINAPI XA2M_DisableEffect(IXAudio2MasteringVoice *iface, UINT32 EffectIndex,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %u, 0x%x\n", This, EffectIndex, OperationSet);
    return FAudioVoice_DisableEffect(This->faudio_voice, EffectIndex, OperationSet);
}

static void WINAPI XA2M_GetEffectState(IXAudio2MasteringVoice *iface, UINT32 EffectIndex,
        BOOL *pEnabled)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %u, %p\n", This, EffectIndex, pEnabled);
    FAudioVoice_GetEffectState(This->faudio_voice, EffectIndex, (int32_t*)pEnabled);
}

static HRESULT WINAPI XA2M_SetEffectParameters(IXAudio2MasteringVoice *iface,
        UINT32 EffectIndex, const void *pParameters, UINT32 ParametersByteSize,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %u, %p, 0x%x, 0x%x\n", This, EffectIndex, pParameters,
            ParametersByteSize, OperationSet);
    return FAudioVoice_SetEffectParameters(This->faudio_voice, EffectIndex,
            pParameters, ParametersByteSize, OperationSet);
}

static HRESULT WINAPI XA2M_GetEffectParameters(IXAudio2MasteringVoice *iface,
        UINT32 EffectIndex, void *pParameters, UINT32 ParametersByteSize)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %u, %p, 0x%x\n", This, EffectIndex, pParameters,
            ParametersByteSize);
    return FAudioVoice_GetEffectParameters(This->faudio_voice, EffectIndex,
            pParameters, ParametersByteSize);
}

static HRESULT WINAPI XA2M_SetFilterParameters(IXAudio2MasteringVoice *iface,
        const XAUDIO2_FILTER_PARAMETERS *pParameters, UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %p, 0x%x\n", This, pParameters, OperationSet);
    return FAudioVoice_SetFilterParameters(This->faudio_voice, (const FAudioFilterParameters *)pParameters,
            OperationSet);
}

static void WINAPI XA2M_GetFilterParameters(IXAudio2MasteringVoice *iface,
        XAUDIO2_FILTER_PARAMETERS *pParameters)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %p\n", This, pParameters);
    FAudioVoice_GetFilterParameters(This->faudio_voice, (FAudioFilterParameters *)pParameters);
}

static HRESULT WINAPI XA2M_SetOutputFilterParameters(IXAudio2MasteringVoice *iface,
        IXAudio2Voice *pDestinationVoice,
        const XAUDIO2_FILTER_PARAMETERS *pParameters, UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %p, 0x%x\n", This, pDestinationVoice, pParameters, OperationSet);

    return FAudioVoice_SetOutputFilterParameters(This->faudio_voice,
            dst ? dst->faudio_voice : NULL, (const FAudioFilterParameters *)pParameters, OperationSet);
}

static void WINAPI XA2M_GetOutputFilterParameters(IXAudio2MasteringVoice *iface,
        IXAudio2Voice *pDestinationVoice,
        XAUDIO2_FILTER_PARAMETERS *pParameters)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %p\n", This, pDestinationVoice, pParameters);

    FAudioVoice_GetOutputFilterParameters(This->faudio_voice,
            dst ? dst->faudio_voice : NULL, (FAudioFilterParameters *)pParameters);
}

static HRESULT WINAPI XA2M_SetVolume(IXAudio2MasteringVoice *iface, float Volume,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %f, 0x%x\n", This, Volume, OperationSet);
    return FAudioVoice_SetVolume(This->faudio_voice, Volume, OperationSet);
}

static void WINAPI XA2M_GetVolume(IXAudio2MasteringVoice *iface, float *pVolume)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %p\n", This, pVolume);
    return FAudioVoice_GetVolume(This->faudio_voice, pVolume);
}

static HRESULT WINAPI XA2M_SetChannelVolumes(IXAudio2MasteringVoice *iface, UINT32 Channels,
        const float *pVolumes, UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %u, %p, 0x%x\n", This, Channels, pVolumes, OperationSet);
    return FAudioVoice_SetChannelVolumes(This->faudio_voice, Channels,
            pVolumes, OperationSet);
}

static void WINAPI XA2M_GetChannelVolumes(IXAudio2MasteringVoice *iface, UINT32 Channels,
        float *pVolumes)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    TRACE("%p, %u, %p\n", This, Channels, pVolumes);
    return FAudioVoice_GetChannelVolumes(This->faudio_voice, Channels,
            pVolumes);
}

static HRESULT WINAPI XA2M_SetOutputMatrix(IXAudio2MasteringVoice *iface,
        IXAudio2Voice *pDestinationVoice, UINT32 SourceChannels,
        UINT32 DestinationChannels, const float *pLevelMatrix,
        UINT32 OperationSet)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %u, %u, %p, 0x%x\n", This, pDestinationVoice,
            SourceChannels, DestinationChannels, pLevelMatrix, OperationSet);

    return FAudioVoice_SetOutputMatrix(This->faudio_voice, dst ? dst->faudio_voice : NULL,
            SourceChannels, DestinationChannels, pLevelMatrix, OperationSet);
}

static void WINAPI XA2M_GetOutputMatrix(IXAudio2MasteringVoice *iface,
        IXAudio2Voice *pDestinationVoice, UINT32 SourceChannels,
        UINT32 DestinationChannels, float *pLevelMatrix)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);
    XA2VoiceImpl *dst = pDestinationVoice ? impl_from_IXAudio2Voice(pDestinationVoice) : NULL;

    TRACE("%p, %p, %u, %u, %p\n", This, pDestinationVoice,
            SourceChannels, DestinationChannels, pLevelMatrix);

    FAudioVoice_GetOutputMatrix(This->faudio_voice, dst ? dst->faudio_voice : NULL,
            SourceChannels, DestinationChannels, pLevelMatrix);
}

static void WINAPI XA2M_DestroyVoice(IXAudio2MasteringVoice *iface)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);

    TRACE("%p\n", This);

    EnterCriticalSection(&This->lock);

    destroy_voice(This);
    pthread_mutex_lock(&This->engine_lock);
    This->engine_params.proc = NULL;
    pthread_cond_broadcast(&This->engine_ready);
    pthread_mutex_unlock(&This->engine_lock);

    WaitForSingleObject(This->engine_thread, INFINITE);
    This->engine_thread = NULL;

    LeaveCriticalSection(&This->lock);
}

static void WINAPI XA2M_GetChannelMask(IXAudio2MasteringVoice *iface,
        DWORD *pChannelMask)
{
    XA2VoiceImpl *This = impl_from_IXAudio2MasteringVoice(iface);

    TRACE("%p, %p\n", This, pChannelMask);

    FAudioMasteringVoice_GetChannelMask(This->faudio_voice, pChannelMask);
}

static const struct IXAudio2MasteringVoiceVtbl XAudio2MasteringVoice_Vtbl = {
    XA2M_GetVoiceDetails,
    XA2M_SetOutputVoices,
    XA2M_SetEffectChain,
    XA2M_EnableEffect,
    XA2M_DisableEffect,
    XA2M_GetEffectState,
    XA2M_SetEffectParameters,
    XA2M_GetEffectParameters,
    XA2M_SetFilterParameters,
    XA2M_GetFilterParameters,
    XA2M_SetOutputFilterParameters,
    XA2M_GetOutputFilterParameters,
    XA2M_SetVolume,
    XA2M_GetVolume,
    XA2M_SetChannelVolumes,
    XA2M_GetChannelVolumes,
    XA2M_SetOutputMatrix,
    XA2M_GetOutputMatrix,
    XA2M_DestroyVoice,
    XA2M_GetChannelMask
};

/* More Common Voice Functions */

static XA2VoiceImpl *impl_from_IXAudio2Voice(IXAudio2Voice *iface)
{
    if(iface->lpVtbl == (void*)&XAudio2SourceVoice_Vtbl)
        return impl_from_IXAudio2SourceVoice((IXAudio2SourceVoice*)iface);
    if(iface->lpVtbl == (void*)&XAudio2MasteringVoice_Vtbl)
        return impl_from_IXAudio2MasteringVoice((IXAudio2MasteringVoice*)iface);
    if(iface->lpVtbl == (void*)&XAudio2SubmixVoice_Vtbl)
        return impl_from_IXAudio2SubmixVoice((IXAudio2SubmixVoice*)iface);
#if XAUDIO2_VER == 0
    if(iface->lpVtbl == (void*)&XAudio20SourceVoice_Vtbl)
        return impl_from_IXAudio20SourceVoice((IXAudio20SourceVoice*)iface);
    if(iface->lpVtbl == (void*)&XAudio20SubmixVoice_Vtbl)
        return impl_from_IXAudio20SubmixVoice((IXAudio20SubmixVoice*)iface);
    if(iface->lpVtbl == (void*)&XAudio20MasteringVoice_Vtbl)
        return impl_from_IXAudio20MasteringVoice((IXAudio20MasteringVoice*)iface);
#elif XAUDIO2_VER <= 3
    if(iface->lpVtbl == (void*)&XAudio23SourceVoice_Vtbl)
        return impl_from_IXAudio23SourceVoice((IXAudio23SourceVoice*)iface);
    if(iface->lpVtbl == (void*)&XAudio23SubmixVoice_Vtbl)
        return impl_from_IXAudio23SubmixVoice((IXAudio23SubmixVoice*)iface);
    if(iface->lpVtbl == (void*)&XAudio23MasteringVoice_Vtbl)
        return impl_from_IXAudio23MasteringVoice((IXAudio23MasteringVoice*)iface);
#elif XAUDIO2_VER <= 7
    if(iface->lpVtbl == (void*)&XAudio27SourceVoice_Vtbl)
        return impl_from_IXAudio27SourceVoice((IXAudio27SourceVoice*)iface);
    if(iface->lpVtbl == (void*)&XAudio27SubmixVoice_Vtbl)
        return impl_from_IXAudio27SubmixVoice((IXAudio27SubmixVoice*)iface);
    if(iface->lpVtbl == (void*)&XAudio27MasteringVoice_Vtbl)
        return impl_from_IXAudio27MasteringVoice((IXAudio27MasteringVoice*)iface);
#endif
    ERR("invalid IXAudio2Voice pointer: %p\n", iface);
    return NULL;
}

/* XAudio2 Engine Implementation */

static inline IXAudio2Impl *impl_from_IXAudio2(IXAudio2 *iface)
{
    return CONTAINING_RECORD(iface, IXAudio2Impl, IXAudio2_iface);
}

static HRESULT WINAPI IXAudio2Impl_QueryInterface(IXAudio2 *iface, REFIID riid,
        void **ppvObject)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), ppvObject);

    if(IsEqualGUID(riid, &IID_IUnknown) ||
#if XAUDIO2_VER == 8
            IsEqualGUID(riid, &IID_IXAudio28) ||
#endif
            IsEqualGUID(riid, &IID_IXAudio2))
        *ppvObject = &This->IXAudio2_iface;
    else if(IsEqualGUID(riid, &IID_IXAudio27)){
        /* all xaudio versions before 28 share an IID */
#if XAUDIO2_VER == 0
        *ppvObject = &This->IXAudio20_iface;
#elif XAUDIO2_VER <= 2
        *ppvObject = &This->IXAudio22_iface;
#elif XAUDIO2_VER <= 3
        *ppvObject = &This->IXAudio23_iface;
#elif XAUDIO2_VER <= 7
        *ppvObject = &This->IXAudio27_iface;
#else
        *ppvObject = NULL;
#endif
    }else
        *ppvObject = NULL;

    if(*ppvObject){
        IUnknown_AddRef((IUnknown*)*ppvObject);
        return S_OK;
    }

    FIXME("(%p)->(%s,%p), not found\n", This,debugstr_guid(riid), ppvObject);

    return E_NOINTERFACE;
}

static ULONG WINAPI IXAudio2Impl_AddRef(IXAudio2 *iface)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);
    ULONG ref = FAudio_AddRef(This->faudio);
    TRACE("(%p)->(): Refcount now %u\n", This, ref);
    return ref;
}

static ULONG WINAPI IXAudio2Impl_Release(IXAudio2 *iface)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);
    ULONG ref = FAudio_Release(This->faudio);

    TRACE("(%p)->(): Refcount now %u\n", This, ref);

    if (!ref) {
        XA2VoiceImpl *v, *v2;

        LIST_FOR_EACH_ENTRY_SAFE(v, v2, &This->voices, XA2VoiceImpl, entry){
            v->lock.DebugInfo->Spare[0] = 0;
            DeleteCriticalSection(&v->lock);
            HeapFree(GetProcessHeap(), 0, v);
        }

        HeapFree(GetProcessHeap(), 0, This->cbs);

        This->lock.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&This->lock);

        HeapFree(GetProcessHeap(), 0, This);
    }
    return ref;
}

static HRESULT WINAPI IXAudio2Impl_RegisterForCallbacks(IXAudio2 *iface,
        IXAudio2EngineCallback *pCallback)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);
    int i;

    TRACE("(%p)->(%p)\n", This, pCallback);

    EnterCriticalSection(&This->lock);

    for(i = 0; i < This->ncbs; ++i){
        if(!This->cbs[i] || This->cbs[i] == pCallback){
            This->cbs[i] = pCallback;
            LeaveCriticalSection(&This->lock);
            return S_OK;
        }
    }

    This->ncbs++;
    This->cbs = heap_realloc(This->cbs, This->ncbs * sizeof(*This->cbs));

    This->cbs[i] = pCallback;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static void WINAPI IXAudio2Impl_UnregisterForCallbacks(IXAudio2 *iface,
        IXAudio2EngineCallback *pCallback)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);
    int i;

    TRACE("(%p)->(%p)\n", This, pCallback);

    EnterCriticalSection(&This->lock);

    if(This->ncbs == 0){
        LeaveCriticalSection(&This->lock);
        return;
    }

    for(i = 0; i < This->ncbs; ++i){
        if(This->cbs[i] == pCallback)
            break;
    }

    for(; i < This->ncbs - 1 && This->cbs[i + 1]; ++i)
        This->cbs[i] = This->cbs[i + 1];

    if(i < This->ncbs)
        This->cbs[i] = NULL;

    LeaveCriticalSection(&This->lock);
}

static inline XA2VoiceImpl *create_voice(IXAudio2Impl *This)
{
    XA2VoiceImpl *voice;

    voice = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*voice));
    if(!voice)
        return NULL;

    list_add_head(&This->voices, &voice->entry);

    voice->IXAudio2SourceVoice_iface.lpVtbl = &XAudio2SourceVoice_Vtbl;
#if XAUDIO2_VER == 0
    voice->IXAudio20SourceVoice_iface.lpVtbl = &XAudio20SourceVoice_Vtbl;
#elif XAUDIO2_VER <= 3
    voice->IXAudio23SourceVoice_iface.lpVtbl = &XAudio23SourceVoice_Vtbl;
#elif XAUDIO2_VER <= 7
    voice->IXAudio27SourceVoice_iface.lpVtbl = &XAudio27SourceVoice_Vtbl;
#endif

    voice->IXAudio2SubmixVoice_iface.lpVtbl = &XAudio2SubmixVoice_Vtbl;
#if XAUDIO2_VER == 0
    voice->IXAudio20SubmixVoice_iface.lpVtbl = &XAudio20SubmixVoice_Vtbl;
#elif XAUDIO2_VER <= 3
    voice->IXAudio23SubmixVoice_iface.lpVtbl = &XAudio23SubmixVoice_Vtbl;
#elif XAUDIO2_VER <= 7
    voice->IXAudio27SubmixVoice_iface.lpVtbl = &XAudio27SubmixVoice_Vtbl;
#endif

    voice->FAudioVoiceCallback_vtbl = FAudioVoiceCallback_Vtbl;

    InitializeCriticalSection(&voice->lock);
    voice->lock.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": XA2VoiceImpl.lock");

    return voice;
}

static HRESULT WINAPI IXAudio2Impl_CreateSourceVoice(IXAudio2 *iface,
        IXAudio2SourceVoice **ppSourceVoice, const WAVEFORMATEX *pSourceFormat,
        UINT32 flags, float maxFrequencyRatio,
        IXAudio2VoiceCallback *pCallback, const XAUDIO2_VOICE_SENDS *pSendList,
        const XAUDIO2_EFFECT_CHAIN *pEffectChain)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);
    XA2VoiceImpl *src;
    HRESULT hr;
    FAudioVoiceSends *faudio_sends;

    TRACE("(%p)->(%p, %p, 0x%x, %f, %p, %p, %p)\n", This, ppSourceVoice,
            pSourceFormat, flags, maxFrequencyRatio, pCallback, pSendList,
            pEffectChain);

    EnterCriticalSection(&This->lock);

    LIST_FOR_EACH_ENTRY(src, &This->voices, XA2VoiceImpl, entry){
        EnterCriticalSection(&src->lock);
        if(!src->in_use)
            break;
        LeaveCriticalSection(&src->lock);
    }

    if(&src->entry == &This->voices){
        src = create_voice(This);
        EnterCriticalSection(&src->lock);
    }

    LeaveCriticalSection(&This->lock);

    src->effect_chain = wrap_effect_chain(pEffectChain);
    faudio_sends = wrap_voice_sends(pSendList);

    hr = FAudio_CreateSourceVoice(This->faudio, &src->faudio_voice,
            (FAudioWaveFormatEx*)pSourceFormat, flags, maxFrequencyRatio,
            &src->FAudioVoiceCallback_vtbl, faudio_sends,
            src->effect_chain);
    free_voice_sends(faudio_sends);
    if(FAILED(hr)){
        LeaveCriticalSection(&This->lock);
        return hr;
    }
    src->in_use = TRUE;
    src->cb = pCallback;

    LeaveCriticalSection(&src->lock);

#if XAUDIO2_VER == 0
    *ppSourceVoice = (IXAudio2SourceVoice*)&src->IXAudio20SourceVoice_iface;
#elif XAUDIO2_VER <= 3
    *ppSourceVoice = (IXAudio2SourceVoice*)&src->IXAudio23SourceVoice_iface;
#elif XAUDIO2_VER <= 7
    *ppSourceVoice = (IXAudio2SourceVoice*)&src->IXAudio27SourceVoice_iface;
#else
    *ppSourceVoice = &src->IXAudio2SourceVoice_iface;
#endif

    TRACE("Created source voice: %p\n", src);

    return S_OK;
}

static HRESULT WINAPI IXAudio2Impl_CreateSubmixVoice(IXAudio2 *iface,
        IXAudio2SubmixVoice **ppSubmixVoice, UINT32 inputChannels,
        UINT32 inputSampleRate, UINT32 flags, UINT32 processingStage,
        const XAUDIO2_VOICE_SENDS *pSendList,
        const XAUDIO2_EFFECT_CHAIN *pEffectChain)
{
    HRESULT hr;
    IXAudio2Impl *This = impl_from_IXAudio2(iface);
    XA2VoiceImpl *sub;
    FAudioVoiceSends *faudio_sends;

    TRACE("(%p)->(%p, %u, %u, 0x%x, %u, %p, %p)\n", This, ppSubmixVoice,
            inputChannels, inputSampleRate, flags, processingStage, pSendList,
            pEffectChain);

    EnterCriticalSection(&This->lock);

    LIST_FOR_EACH_ENTRY(sub, &This->voices, XA2VoiceImpl, entry){
        EnterCriticalSection(&sub->lock);
        if(!sub->in_use)
            break;
        LeaveCriticalSection(&sub->lock);
    }

    if(&sub->entry == &This->voices){
        sub = create_voice(This);
        EnterCriticalSection(&sub->lock);
    }

    LeaveCriticalSection(&This->lock);

    sub->effect_chain = wrap_effect_chain(pEffectChain);
    faudio_sends = wrap_voice_sends(pSendList);

    hr = FAudio_CreateSubmixVoice(This->faudio, &sub->faudio_voice, inputChannels,
            inputSampleRate, flags, processingStage, faudio_sends,
            sub->effect_chain);
    free_voice_sends(faudio_sends);
    if(FAILED(hr)){
        LeaveCriticalSection(&sub->lock);
        return hr;
    }
    sub->in_use = TRUE;

    LeaveCriticalSection(&sub->lock);

#if XAUDIO2_VER == 0
    *ppSubmixVoice = (IXAudio2SubmixVoice*)&sub->IXAudio20SubmixVoice_iface;
#elif XAUDIO2_VER <= 3
    *ppSubmixVoice = (IXAudio2SubmixVoice*)&sub->IXAudio23SubmixVoice_iface;
#elif XAUDIO2_VER <= 7
    *ppSubmixVoice = (IXAudio2SubmixVoice*)&sub->IXAudio27SubmixVoice_iface;
#else
    *ppSubmixVoice = &sub->IXAudio2SubmixVoice_iface;
#endif

    TRACE("Created submix voice: %p\n", sub);

    return S_OK;
}

/* called thread created by SDL, must not access Wine TEB */
void engine_cb(FAudioEngineCallEXT proc, FAudio *faudio, float *stream, void *user)
{
    XA2VoiceImpl *This = user;

    pthread_mutex_lock(&This->engine_lock);

    This->engine_params.proc = proc;
    This->engine_params.stream = stream;
    This->engine_params.faudio = faudio;

    pthread_cond_broadcast(&This->engine_ready);

    while(This->engine_params.proc)
        pthread_cond_wait(&This->engine_done, &This->engine_lock);

    pthread_mutex_unlock(&This->engine_lock);
}

/* wine thread, OK to access TEB, invoke client code, etc */
DWORD WINAPI engine_thread(void *user)
{
    XA2VoiceImpl *This = user;

    pthread_mutex_lock(&This->engine_lock);

    pthread_cond_broadcast(&This->engine_done);

    do{
        pthread_cond_wait(&This->engine_ready, &This->engine_lock);

        if(This->engine_params.proc){
            This->engine_params.proc(This->engine_params.faudio, This->engine_params.stream);
            This->engine_params.proc = NULL;
            pthread_cond_broadcast(&This->engine_done);
        }
    }while(This->in_use);

    pthread_mutex_unlock(&This->engine_lock);

    return 0;
}

static HRESULT WINAPI IXAudio2Impl_CreateMasteringVoice(IXAudio2 *iface,
        IXAudio2MasteringVoice **ppMasteringVoice, UINT32 inputChannels,
        UINT32 inputSampleRate, UINT32 flags, const WCHAR *deviceId,
        const XAUDIO2_EFFECT_CHAIN *pEffectChain,
        AUDIO_STREAM_CATEGORY streamCategory)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);

    TRACE("(%p)->(%p, %u, %u, 0x%x, %s, %p, 0x%x)\n", This,
            ppMasteringVoice, inputChannels, inputSampleRate, flags,
            wine_dbgstr_w(deviceId), pEffectChain, streamCategory);

    EnterCriticalSection(&This->lock);

    /* Note that we don't have paths for each XAUDIO2_VER here.
     * All versions < 8 have a very different CreateMasteringVoice, so we
     * implement those separately in compat.c.
     * -flibit
     */
    *ppMasteringVoice = &This->mst.IXAudio2MasteringVoice_iface;

    EnterCriticalSection(&This->mst.lock);

    if(This->mst.in_use){
        LeaveCriticalSection(&This->mst.lock);
        LeaveCriticalSection(&This->lock);
        return COMPAT_E_INVALID_CALL;
    }

    LeaveCriticalSection(&This->lock);

    This->mst.effect_chain = wrap_effect_chain(pEffectChain);

    pthread_mutex_lock(&This->mst.engine_lock);

    This->mst.engine_thread = CreateThread(NULL, 0, &engine_thread, &This->mst, 0, NULL);

    pthread_cond_wait(&This->mst.engine_done, &This->mst.engine_lock);

    pthread_mutex_unlock(&This->mst.engine_lock);

    FAudio_SetEngineProcedureEXT(This->faudio, &engine_cb, &This->mst);

    FAudio_CreateMasteringVoice8(This->faudio, &This->mst.faudio_voice, inputChannels,
            inputSampleRate, flags, NULL /* TODO: (uint16_t*)deviceId */,
            This->mst.effect_chain, (FAudioStreamCategory)streamCategory);

    This->mst.in_use = TRUE;

    LeaveCriticalSection(&This->mst.lock);

    return S_OK;
}

static HRESULT WINAPI IXAudio2Impl_StartEngine(IXAudio2 *iface)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);

    TRACE("(%p)->()\n", This);

    return FAudio_StartEngine(This->faudio);
}

static void WINAPI IXAudio2Impl_StopEngine(IXAudio2 *iface)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);

    TRACE("(%p)->()\n", This);

    FAudio_StopEngine(This->faudio);
}

static HRESULT WINAPI IXAudio2Impl_CommitChanges(IXAudio2 *iface,
        UINT32 operationSet)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);

    TRACE("(%p)->(0x%x): stub!\n", This, operationSet);

    return FAudio_CommitChanges(This->faudio);
}

static void WINAPI IXAudio2Impl_GetPerformanceData(IXAudio2 *iface,
        XAUDIO2_PERFORMANCE_DATA *pPerfData)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);

    TRACE("(%p)->(%p): stub!\n", This, pPerfData);

    FAudio_GetPerformanceData(This->faudio, (FAudioPerformanceData *)pPerfData);
}

static void WINAPI IXAudio2Impl_SetDebugConfiguration(IXAudio2 *iface,
        const XAUDIO2_DEBUG_CONFIGURATION *pDebugConfiguration,
        void *pReserved)
{
    IXAudio2Impl *This = impl_from_IXAudio2(iface);

    TRACE("(%p)->(%p, %p): stub!\n", This, pDebugConfiguration, pReserved);

    FAudio_SetDebugConfiguration(This->faudio, (FAudioDebugConfiguration *)pDebugConfiguration, pReserved);
}

/* XAudio2 2.8 */
static const IXAudio2Vtbl XAudio2_Vtbl =
{
    IXAudio2Impl_QueryInterface,
    IXAudio2Impl_AddRef,
    IXAudio2Impl_Release,
    IXAudio2Impl_RegisterForCallbacks,
    IXAudio2Impl_UnregisterForCallbacks,
    IXAudio2Impl_CreateSourceVoice,
    IXAudio2Impl_CreateSubmixVoice,
    IXAudio2Impl_CreateMasteringVoice,
    IXAudio2Impl_StartEngine,
    IXAudio2Impl_StopEngine,
    IXAudio2Impl_CommitChanges,
    IXAudio2Impl_GetPerformanceData,
    IXAudio2Impl_SetDebugConfiguration
};

/* XAudio2 ClassFactory */

struct xaudio2_cf {
    IClassFactory IClassFactory_iface;
    LONG ref;
};

static struct xaudio2_cf *impl_from_IClassFactory(IClassFactory *iface)
{
    return CONTAINING_RECORD(iface, struct xaudio2_cf, IClassFactory_iface);
}

static HRESULT WINAPI XAudio2CF_QueryInterface(IClassFactory *iface, REFIID riid, void **ppobj)
{
    if(IsEqualGUID(riid, &IID_IUnknown)
            || IsEqualGUID(riid, &IID_IClassFactory))
    {
        IClassFactory_AddRef(iface);
        *ppobj = iface;
        return S_OK;
    }

    *ppobj = NULL;
    WARN("(%p)->(%s, %p): interface not found\n", iface, debugstr_guid(riid), ppobj);
    return E_NOINTERFACE;
}

static ULONG WINAPI XAudio2CF_AddRef(IClassFactory *iface)
{
    struct xaudio2_cf *This = impl_from_IClassFactory(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p)->(): Refcount now %u\n", This, ref);
    return ref;
}

static ULONG WINAPI XAudio2CF_Release(IClassFactory *iface)
{
    struct xaudio2_cf *This = impl_from_IClassFactory(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p)->(): Refcount now %u\n", This, ref);
    if (!ref)
        HeapFree(GetProcessHeap(), 0, This);
    return ref;
}

static HRESULT WINAPI XAudio2CF_CreateInstance(IClassFactory *iface, IUnknown *pOuter,
                                               REFIID riid, void **ppobj)
{
    struct xaudio2_cf *This = impl_from_IClassFactory(iface);
    HRESULT hr;
    IXAudio2Impl *object;

    TRACE("(%p)->(%p,%s,%p)\n", This, pOuter, debugstr_guid(riid), ppobj);

    *ppobj = NULL;

    if(pOuter)
        return CLASS_E_NOAGGREGATION;

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if(!object)
        return E_OUTOFMEMORY;

    object->IXAudio2_iface.lpVtbl = &XAudio2_Vtbl;

#if XAUDIO2_VER == 0
    object->IXAudio20_iface.lpVtbl = &XAudio20_Vtbl;
#elif XAUDIO2_VER <= 2
    object->IXAudio22_iface.lpVtbl = &XAudio22_Vtbl;
#elif XAUDIO2_VER <= 3
    object->IXAudio23_iface.lpVtbl = &XAudio23_Vtbl;
#elif XAUDIO2_VER <= 7
    object->IXAudio27_iface.lpVtbl = &XAudio27_Vtbl;
#endif

    object->mst.IXAudio2MasteringVoice_iface.lpVtbl = &XAudio2MasteringVoice_Vtbl;

#if XAUDIO2_VER == 0
    object->mst.IXAudio20MasteringVoice_iface.lpVtbl = &XAudio20MasteringVoice_Vtbl;
#elif XAUDIO2_VER <= 3
    object->mst.IXAudio23MasteringVoice_iface.lpVtbl = &XAudio23MasteringVoice_Vtbl;
#elif XAUDIO2_VER <= 7
    object->mst.IXAudio27MasteringVoice_iface.lpVtbl = &XAudio27MasteringVoice_Vtbl;
#endif

    object->FAudioEngineCallback_vtbl = FAudioEngineCallback_Vtbl;

    list_init(&object->voices);

    InitializeCriticalSection(&object->lock);
    object->lock.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": IXAudio2Impl.lock");

    InitializeCriticalSection(&object->mst.lock);
    object->mst.lock.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": XA2MasteringVoice.lock");

    pthread_mutex_init(&object->mst.engine_lock, NULL);
    pthread_cond_init(&object->mst.engine_done, NULL);
    pthread_cond_init(&object->mst.engine_ready, NULL);

    FAudioCOMConstructWithCustomAllocatorEXT(
        &object->faudio,
        XAUDIO2_VER,
        XAudio_Internal_Malloc,
        XAudio_Internal_Free,
        XAudio_Internal_Realloc
    );

    FAudio_RegisterForCallbacks(object->faudio, &object->FAudioEngineCallback_vtbl);

    hr = IXAudio2_QueryInterface(&object->IXAudio2_iface, riid, ppobj);
    if(FAILED(hr)){
        object->lock.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&object->lock);
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }

    TRACE("Created XAudio version %u: %p\n", 20 + XAUDIO2_VER, object);

    return hr;
}

static HRESULT WINAPI XAudio2CF_LockServer(IClassFactory *iface, BOOL dolock)
{
    struct xaudio2_cf *This = impl_from_IClassFactory(iface);
    FIXME("(%p)->(%d): stub!\n", This, dolock);
    return S_OK;
}

static const IClassFactoryVtbl XAudio2CF_Vtbl =
{
    XAudio2CF_QueryInterface,
    XAudio2CF_AddRef,
    XAudio2CF_Release,
    XAudio2CF_CreateInstance,
    XAudio2CF_LockServer
};

/* Engine Generators */

static inline HRESULT make_xaudio2_factory(REFIID riid, void **ppv)
{
    HRESULT hr;
    struct xaudio2_cf *ret = HeapAlloc(GetProcessHeap(), 0, sizeof(struct xaudio2_cf));
    ret->IClassFactory_iface.lpVtbl = &XAudio2CF_Vtbl;
    ret->ref = 0;
    hr = IClassFactory_QueryInterface(&ret->IClassFactory_iface, riid, ppv);
    if(FAILED(hr))
        HeapFree(GetProcessHeap(), 0, ret);
    return hr;
}

HRESULT xaudio2_initialize(IXAudio2Impl *This, UINT32 flags, XAUDIO2_PROCESSOR proc)
{
    if(proc != XAUDIO2_ANY_PROCESSOR)
        WARN("Processor affinity not implemented in FAudio\n");
    return FAudio_Initialize(This->faudio, flags, FAUDIO_DEFAULT_PROCESSOR);
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    TRACE("(%s, %s, %p)\n", debugstr_guid(rclsid), debugstr_guid(riid), ppv);

    if(IsEqualGUID(rclsid, &CLSID_XAudio20) ||
            IsEqualGUID(rclsid, &CLSID_XAudio21) ||
            IsEqualGUID(rclsid, &CLSID_XAudio22) ||
            IsEqualGUID(rclsid, &CLSID_XAudio23) ||
            IsEqualGUID(rclsid, &CLSID_XAudio24) ||
            IsEqualGUID(rclsid, &CLSID_XAudio25) ||
            IsEqualGUID(rclsid, &CLSID_XAudio26) ||
            IsEqualGUID(rclsid, &CLSID_XAudio27))
        return make_xaudio2_factory(riid, ppv);

    if(IsEqualGUID(rclsid, &CLSID_AudioVolumeMeter20) ||
                IsEqualGUID(rclsid, &CLSID_AudioVolumeMeter21) ||
                IsEqualGUID(rclsid, &CLSID_AudioVolumeMeter22) ||
                IsEqualGUID(rclsid, &CLSID_AudioVolumeMeter23) ||
                IsEqualGUID(rclsid, &CLSID_AudioVolumeMeter24) ||
                IsEqualGUID(rclsid, &CLSID_AudioVolumeMeter25) ||
                IsEqualGUID(rclsid, &CLSID_AudioVolumeMeter26) ||
                IsEqualGUID(rclsid, &CLSID_AudioVolumeMeter27))
        return make_xapo_factory(&CLSID_AudioVolumeMeter27, riid, ppv);

    if(IsEqualGUID(rclsid, &CLSID_AudioReverb20) ||
                IsEqualGUID(rclsid, &CLSID_AudioReverb21) ||
                IsEqualGUID(rclsid, &CLSID_AudioReverb22) ||
                IsEqualGUID(rclsid, &CLSID_AudioReverb23) ||
                IsEqualGUID(rclsid, &CLSID_AudioReverb24) ||
                IsEqualGUID(rclsid, &CLSID_AudioReverb25) ||
                IsEqualGUID(rclsid, &CLSID_AudioReverb26) ||
                IsEqualGUID(rclsid, &CLSID_AudioReverb27))
        return make_xapo_factory(&CLSID_AudioReverb27, riid, ppv);

    return CLASS_E_CLASSNOTAVAILABLE;
}

#if XAUDIO2_VER >= 8
HRESULT WINAPI XAudio2Create(IXAudio2 **ppxa2, UINT32 flags, XAUDIO2_PROCESSOR proc)
{
    HRESULT hr;
    IXAudio2 *xa2;
    IClassFactory *cf;

    TRACE("%p 0x%x 0x%x\n", ppxa2, flags, proc);

    hr = make_xaudio2_factory(&IID_IClassFactory, (void**)&cf);
    if(FAILED(hr))
        return hr;

    hr = IClassFactory_CreateInstance(cf, NULL, &IID_IXAudio2, (void**)&xa2);
    IClassFactory_Release(cf);
    if(FAILED(hr))
        return hr;

    hr = xaudio2_initialize(impl_from_IXAudio2(xa2), flags, proc);
    if(FAILED(hr)){
        IXAudio2_Release(xa2);
        return hr;
    }

    *ppxa2 = xa2;

    return S_OK;
}

HRESULT WINAPI CreateAudioVolumeMeter(IUnknown **out)
{
    IClassFactory *cf;
    HRESULT hr;

    hr = make_xapo_factory(&CLSID_AudioVolumeMeter27, &IID_IClassFactory, (void**)&cf);
    if(FAILED(hr))
        return hr;

    hr = IClassFactory_CreateInstance(cf, NULL, &IID_IUnknown, (void**)out);

    IClassFactory_Release(cf);

    return hr;
}

HRESULT WINAPI CreateAudioReverb(IUnknown **out)
{
    IClassFactory *cf;
    HRESULT hr;

    hr = make_xapo_factory(&CLSID_AudioReverb27, &IID_IClassFactory, (void**)&cf);
    if(FAILED(hr))
        return hr;

    hr = IClassFactory_CreateInstance(cf, NULL, &IID_IUnknown, (void**)out);

    IClassFactory_Release(cf);

    return hr;
}
#endif /* XAUDIO2_VER >= 8 */
