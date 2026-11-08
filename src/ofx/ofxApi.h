#pragma once
// OpenFX image effect API: the C declarations a host needs (types, suites,
// actions, property names). Written from the public OpenFX specification
// (https://openfx.readthedocs.io), which defines a stable C ABI: suite
// layouts and string constants below must match it exactly.
#include <cstddef>

extern "C" {

typedef int OfxStatus;
typedef double OfxTime;
typedef struct OfxPropertySetStruct *OfxPropertySetHandle;
typedef struct OfxParamStruct *OfxParamHandle;
typedef struct OfxParamSetStruct *OfxParamSetHandle;
typedef struct OfxImageEffectStruct *OfxImageEffectHandle;
typedef struct OfxImageClipStruct *OfxImageClipHandle;
typedef struct OfxImageMemoryStruct *OfxImageMemoryHandle;
typedef struct OfxMutex *OfxMutexHandle;

typedef struct OfxRectI { int x1, y1, x2, y2; } OfxRectI;
typedef struct OfxRectD { double x1, y1, x2, y2; } OfxRectD;
typedef struct OfxPointD { double x, y; } OfxPointD;
typedef struct OfxPointI { int x, y; } OfxPointI;
typedef struct OfxRangeD { double min, max; } OfxRangeD;
typedef struct OfxRangeI { int min, max; } OfxRangeI;
typedef struct OfxRGBAColourF { float r, g, b, a; } OfxRGBAColourF;

typedef struct OfxHost {
    OfxPropertySetHandle host;
    const void *(*fetchSuite)(OfxPropertySetHandle host, const char *suiteName, int suiteVersion);
} OfxHost;

typedef OfxStatus(OfxPluginEntryPoint)(const char *action, const void *handle, OfxPropertySetHandle inArgs,
                                       OfxPropertySetHandle outArgs);

typedef struct OfxPlugin {
    const char *pluginApi;
    int apiVersion;
    const char *pluginIdentifier;
    unsigned int pluginVersionMajor;
    unsigned int pluginVersionMinor;
    void (*setHost)(OfxHost *host);
    OfxPluginEntryPoint *mainEntry;
} OfxPlugin;

typedef int (*OfxGetNumberOfPluginsFn)(void);
typedef OfxPlugin *(*OfxGetPluginFn)(int nth);
typedef OfxStatus (*OfxSetHostFn)(const OfxHost *host);

typedef struct OfxPropertySuiteV1 {
    OfxStatus (*propSetPointer)(OfxPropertySetHandle properties, const char *property, int index, void *value);
    OfxStatus (*propSetString)(OfxPropertySetHandle properties, const char *property, int index, const char *value);
    OfxStatus (*propSetDouble)(OfxPropertySetHandle properties, const char *property, int index, double value);
    OfxStatus (*propSetInt)(OfxPropertySetHandle properties, const char *property, int index, int value);
    OfxStatus (*propSetPointerN)(OfxPropertySetHandle properties, const char *property, int count, void *const *value);
    OfxStatus (*propSetStringN)(OfxPropertySetHandle properties, const char *property, int count, const char *const *value);
    OfxStatus (*propSetDoubleN)(OfxPropertySetHandle properties, const char *property, int count, const double *value);
    OfxStatus (*propSetIntN)(OfxPropertySetHandle properties, const char *property, int count, const int *value);
    OfxStatus (*propGetPointer)(OfxPropertySetHandle properties, const char *property, int index, void **value);
    OfxStatus (*propGetString)(OfxPropertySetHandle properties, const char *property, int index, char **value);
    OfxStatus (*propGetDouble)(OfxPropertySetHandle properties, const char *property, int index, double *value);
    OfxStatus (*propGetInt)(OfxPropertySetHandle properties, const char *property, int index, int *value);
    OfxStatus (*propGetPointerN)(OfxPropertySetHandle properties, const char *property, int count, void **value);
    OfxStatus (*propGetStringN)(OfxPropertySetHandle properties, const char *property, int count, char **value);
    OfxStatus (*propGetDoubleN)(OfxPropertySetHandle properties, const char *property, int count, double *value);
    OfxStatus (*propGetIntN)(OfxPropertySetHandle properties, const char *property, int count, int *value);
    OfxStatus (*propReset)(OfxPropertySetHandle properties, const char *property);
    OfxStatus (*propGetDimension)(OfxPropertySetHandle properties, const char *property, int *count);
} OfxPropertySuiteV1;

typedef struct OfxImageEffectSuiteV1 {
    OfxStatus (*getPropertySet)(OfxImageEffectHandle imageEffect, OfxPropertySetHandle *propHandle);
    OfxStatus (*getParamSet)(OfxImageEffectHandle imageEffect, OfxParamSetHandle *paramSet);
    OfxStatus (*clipDefine)(OfxImageEffectHandle imageEffect, const char *name, OfxPropertySetHandle *propertySet);
    OfxStatus (*clipGetHandle)(OfxImageEffectHandle imageEffect, const char *name, OfxImageClipHandle *clip,
                               OfxPropertySetHandle *propertySet);
    OfxStatus (*clipGetPropertySet)(OfxImageClipHandle clip, OfxPropertySetHandle *propHandle);
    OfxStatus (*clipGetImage)(OfxImageClipHandle clip, OfxTime time, const OfxRectD *region, OfxPropertySetHandle *imageHandle);
    OfxStatus (*clipReleaseImage)(OfxPropertySetHandle imageHandle);
    OfxStatus (*clipGetRegionOfDefinition)(OfxImageClipHandle clip, OfxTime time, OfxRectD *bounds);
    int (*abort)(OfxImageEffectHandle imageEffect);
    OfxStatus (*imageMemoryAlloc)(OfxImageEffectHandle instanceHandle, size_t nBytes, OfxImageMemoryHandle *memoryHandle);
    OfxStatus (*imageMemoryFree)(OfxImageMemoryHandle memoryHandle);
    OfxStatus (*imageMemoryLock)(OfxImageMemoryHandle memoryHandle, void **returnedPtr);
    OfxStatus (*imageMemoryUnlock)(OfxImageMemoryHandle memoryHandle);
} OfxImageEffectSuiteV1;

typedef struct OfxParameterSuiteV1 {
    OfxStatus (*paramDefine)(OfxParamSetHandle paramSet, const char *paramType, const char *name, OfxPropertySetHandle *propertySet);
    OfxStatus (*paramGetHandle)(OfxParamSetHandle paramSet, const char *name, OfxParamHandle *param, OfxPropertySetHandle *propertySet);
    OfxStatus (*paramSetGetPropertySet)(OfxParamSetHandle paramSet, OfxPropertySetHandle *propHandle);
    OfxStatus (*paramGetPropertySet)(OfxParamHandle param, OfxPropertySetHandle *propHandle);
    OfxStatus (*paramGetValue)(OfxParamHandle paramHandle, ...);
    OfxStatus (*paramGetValueAtTime)(OfxParamHandle paramHandle, OfxTime time, ...);
    OfxStatus (*paramGetDerivative)(OfxParamHandle paramHandle, OfxTime time, ...);
    OfxStatus (*paramGetIntegral)(OfxParamHandle paramHandle, OfxTime time1, OfxTime time2, ...);
    OfxStatus (*paramSetValue)(OfxParamHandle paramHandle, ...);
    OfxStatus (*paramSetValueAtTime)(OfxParamHandle paramHandle, OfxTime time, ...);
    OfxStatus (*paramGetNumKeys)(OfxParamHandle paramHandle, unsigned int *numberOfKeys);
    OfxStatus (*paramGetKeyTime)(OfxParamHandle paramHandle, unsigned int nthKey, OfxTime *time);
    OfxStatus (*paramGetKeyIndex)(OfxParamHandle paramHandle, OfxTime time, int direction, int *index);
    OfxStatus (*paramDeleteKey)(OfxParamHandle paramHandle, OfxTime time);
    OfxStatus (*paramDeleteAllKeys)(OfxParamHandle paramHandle);
    OfxStatus (*paramCopy)(OfxParamHandle paramTo, OfxParamHandle paramFrom, OfxTime dstOffset, const OfxRangeD *frameRange);
    OfxStatus (*paramEditBegin)(OfxParamSetHandle paramSet, const char *name);
    OfxStatus (*paramEditEnd)(OfxParamSetHandle paramSet);
} OfxParameterSuiteV1;

typedef struct OfxMemorySuiteV1 {
    OfxStatus (*memoryAlloc)(void *handle, size_t nBytes, void **allocatedData);
    OfxStatus (*memoryFree)(void *allocatedData);
} OfxMemorySuiteV1;

typedef void(OfxThreadFunctionV1)(unsigned int threadIndex, unsigned int threadMax, void *customArg);

typedef struct OfxMultiThreadSuiteV1 {
    OfxStatus (*multiThread)(OfxThreadFunctionV1 func, unsigned int nThreads, void *customArg);
    OfxStatus (*multiThreadNumCPUs)(unsigned int *nCPUs);
    OfxStatus (*multiThreadIndex)(unsigned int *threadIndex);
    int (*multiThreadIsSpawnedThread)(void);
    OfxStatus (*mutexCreate)(OfxMutexHandle *mutex, int lockCount);
    OfxStatus (*mutexDestroy)(const OfxMutexHandle mutex);
    OfxStatus (*mutexLock)(const OfxMutexHandle mutex);
    OfxStatus (*mutexUnLock)(const OfxMutexHandle mutex);
    OfxStatus (*mutexTryLock)(const OfxMutexHandle mutex);
} OfxMultiThreadSuiteV1;

typedef struct OfxMessageSuiteV1 {
    OfxStatus (*message)(void *handle, const char *messageType, const char *messageId, const char *format, ...);
} OfxMessageSuiteV1;

typedef struct OfxMessageSuiteV2 {
    OfxStatus (*message)(void *handle, const char *messageType, const char *messageId, const char *format, ...);
    OfxStatus (*setPersistentMessage)(void *handle, const char *messageType, const char *messageId, const char *format, ...);
    OfxStatus (*clearPersistentMessage)(void *handle);
} OfxMessageSuiteV2;

typedef struct OfxProgressSuiteV1 {
    OfxStatus (*progressStart)(void *effectInstance, const char *label);
    OfxStatus (*progressUpdate)(void *effectInstance, double progress);
    OfxStatus (*progressEnd)(void *effectInstance);
} OfxProgressSuiteV1;

typedef struct OfxProgressSuiteV2 {
    OfxStatus (*progressStart)(void *effectInstance, const char *message, const char *messageid);
    OfxStatus (*progressUpdate)(void *effectInstance, double progress);
    OfxStatus (*progressEnd)(void *effectInstance);
} OfxProgressSuiteV2;

typedef struct OfxTimeLineSuiteV1 {
    OfxStatus (*getTime)(void *instance, double *time);
    OfxStatus (*gotoTime)(void *instance, double time);
    OfxStatus (*getTimeBounds)(void *instance, double *firstTime, double *lastTime);
} OfxTimeLineSuiteV1;

} // extern "C"

// Status codes
#define kOfxStatOK 0
#define kOfxStatFailed ((int)1)
#define kOfxStatErrFatal ((int)2)
#define kOfxStatErrUnknown ((int)3)
#define kOfxStatErrMissingHostFeature ((int)4)
#define kOfxStatErrUnsupported ((int)5)
#define kOfxStatErrExists ((int)6)
#define kOfxStatErrFormat ((int)7)
#define kOfxStatErrMemory ((int)8)
#define kOfxStatErrBadHandle ((int)9)
#define kOfxStatErrBadIndex ((int)10)
#define kOfxStatErrValue ((int)11)
#define kOfxStatReplyYes ((int)12)
#define kOfxStatReplyNo ((int)13)
#define kOfxStatReplyDefault ((int)14)
#define kOfxStatErrImageFormat ((int)1000)

// Suites
#define kOfxPropertySuite "OfxPropertySuite"
#define kOfxImageEffectSuite "OfxImageEffectSuite"
#define kOfxParameterSuite "OfxParameterSuite"
#define kOfxMemorySuite "OfxMemorySuite"
#define kOfxMultiThreadSuite "OfxMultiThreadSuite"
#define kOfxMessageSuite "OfxMessageSuite"
#define kOfxProgressSuite "OfxProgressSuite"
#define kOfxTimeLineSuite "OfxTimeLineSuite"

// APIs, types
#define kOfxImageEffectPluginApi "OfxImageEffectPluginAPI"
#define kOfxTypeImageEffectHost "OfxTypeImageEffectHost"
#define kOfxTypeImageEffect "OfxTypeImageEffect"
#define kOfxTypeImageEffectInstance "OfxTypeImageEffectInstance"
#define kOfxTypeParameter "OfxTypeParameter"
#define kOfxTypeParameterInstance "OfxTypeParameterInstance"
#define kOfxTypeClip "OfxTypeClip"
#define kOfxTypeImage "OfxTypeImage"

// Actions
#define kOfxActionLoad "OfxActionLoad"
#define kOfxActionDescribe "OfxActionDescribe"
#define kOfxActionUnload "OfxActionUnload"
#define kOfxActionCreateInstance "OfxActionCreateInstance"
#define kOfxActionDestroyInstance "OfxActionDestroyInstance"
#define kOfxActionInstanceChanged "OfxActionInstanceChanged"
#define kOfxActionBeginInstanceChanged "OfxActionBeginInstanceChanged"
#define kOfxActionEndInstanceChanged "OfxActionEndInstanceChanged"
#define kOfxImageEffectActionDescribeInContext "OfxImageEffectActionDescribeInContext"
#define kOfxImageEffectActionGetRegionOfDefinition "OfxImageEffectActionGetRegionOfDefinition"
#define kOfxImageEffectActionGetRegionsOfInterest "OfxImageEffectActionGetRegionsOfInterest"
#define kOfxImageEffectActionGetClipPreferences "OfxImageEffectActionGetClipPreferences"
#define kOfxImageEffectActionIsIdentity "OfxImageEffectActionIsIdentity"
#define kOfxImageEffectActionRender "OfxImageEffectActionRender"
#define kOfxImageEffectActionBeginSequenceRender "OfxImageEffectActionBeginSequenceRender"
#define kOfxImageEffectActionEndSequenceRender "OfxImageEffectActionEndSequenceRender"

// Generic properties
#define kOfxPropAPIVersion "OfxPropAPIVersion"
#define kOfxPropType "OfxPropType"
#define kOfxPropName "OfxPropName"
#define kOfxPropLabel "OfxPropLabel"
#define kOfxPropShortLabel "OfxPropShortLabel"
#define kOfxPropLongLabel "OfxPropLongLabel"
#define kOfxPropVersion "OfxPropVersion"
#define kOfxPropVersionLabel "OfxPropVersionLabel"
#define kOfxPropPluginDescription "OfxPropPluginDescription"
#define kOfxPropTime "OfxPropTime"
#define kOfxPropIsInteractive "OfxPropIsInteractive"
#define kOfxPropInstanceData "OfxPropInstanceData"
#define kOfxPropChangeReason "OfxPropChangeReason"
#define kOfxPropHostOSHandle "OfxPropHostOSHandle"
#define kOfxChangeUserEdited "OfxChangeUserEdited"
#define kOfxChangePluginEdited "OfxChangePluginEdited"
#define kOfxChangeTime "OfxChangeTime"

// Host properties
#define kOfxImageEffectHostPropIsBackground "OfxImageEffectHostPropIsBackground"
#define kOfxImageEffectHostPropNativeOrigin "OfxImageEffectHostPropNativeOrigin"
#define kOfxHostNativeOriginBottomLeft "OfxImageEffectHostPropNativeOriginBottomLeft"
#define kOfxImageEffectPropSupportsOverlays "OfxImageEffectPropSupportsOverlays"
#define kOfxImageEffectPropSupportsMultiResolution "OfxImageEffectPropSupportsMultiResolution"
#define kOfxImageEffectPropSupportsTiles "OfxImageEffectPropSupportsTiles"
#define kOfxImageEffectPropTemporalClipAccess "OfxImageEffectPropTemporalClipAccess"
#define kOfxImageEffectPropSupportedComponents "OfxImageEffectPropSupportedComponents"
#define kOfxImageEffectPropSupportedContexts "OfxImageEffectPropSupportedContexts"
#define kOfxImageEffectPropSupportedPixelDepths "OfxImageEffectPropSupportedPixelDepths"
#define kOfxImageEffectPropSupportsMultipleClipDepths "OfxImageEffectPropSupportsMultipleClipDepths"
#define kOfxImageEffectPropSupportsMultipleClipPARs "OfxImageEffectPropSupportsMultipleClipPARs"
#define kOfxImageEffectPropSetableFrameRate "OfxImageEffectPropSetableFrameRate"
#define kOfxImageEffectPropSetableFielding "OfxImageEffectPropSetableFielding"
#define kOfxImageEffectInstancePropSequentialRender "OfxImageEffectInstancePropSequentialRender"
#define kOfxImageEffectPropRenderQualityDraft "OfxImageEffectPropRenderQualityDraft"
#define kOfxImageEffectPropOpenGLRenderSupported "OfxImageEffectPropOpenGLRenderSupported"
#define kOfxImageEffectPropCudaRenderSupported "OfxImageEffectPropCudaRenderSupported"
#define kOfxImageEffectPropOpenCLRenderSupported "OfxImageEffectPropOpenCLRenderSupported"
#define kOfxImageEffectPropMetalRenderSupported "OfxImageEffectPropMetalRenderSupported"
#define kOfxParamHostPropSupportsCustomInteract "OfxParamHostPropSupportsCustomInteract"
#define kOfxParamHostPropSupportsStringAnimation "OfxParamHostPropSupportsStringAnimation"
#define kOfxParamHostPropSupportsChoiceAnimation "OfxParamHostPropSupportsChoiceAnimation"
#define kOfxParamHostPropSupportsBooleanAnimation "OfxParamHostPropSupportsBooleanAnimation"
#define kOfxParamHostPropSupportsCustomAnimation "OfxParamHostPropSupportsCustomAnimation"
#define kOfxParamHostPropSupportsStrChoice "OfxParamHostPropSupportsStrChoice"
#define kOfxParamHostPropSupportsStrChoiceAnimation "OfxParamHostPropSupportsStrChoiceAnimation"
#define kOfxParamHostPropSupportsParametricAnimation "OfxParamHostPropSupportsParametricAnimation"
#define kOfxParamHostPropMaxParameters "OfxParamHostPropMaxParameters"
#define kOfxParamHostPropMaxPages "OfxParamHostPropMaxPages"
#define kOfxParamHostPropPageRowColumnCount "OfxParamHostPropPageRowColumnCount"

// Effect properties
#define kOfxImageEffectPropContext "OfxImageEffectPropContext"
#define kOfxImageEffectContextFilter "OfxImageEffectContextFilter"
#define kOfxImageEffectContextGeneral "OfxImageEffectContextGeneral"
#define kOfxImageEffectContextGenerator "OfxImageEffectContextGenerator"
#define kOfxImageEffectContextTransition "OfxImageEffectContextTransition"
#define kOfxImageEffectPluginPropGrouping "OfxImageEffectPluginPropGrouping"
#define kOfxImageEffectPluginPropSingleInstance "OfxImageEffectPluginPropSingleInstance"
#define kOfxImageEffectPluginRenderThreadSafety "OfxImageEffectPluginRenderThreadSafety"
#define kOfxImageEffectPluginPropHostFrameThreading "OfxImageEffectPluginPropHostFrameThreading"
#define kOfxImageEffectPluginPropFieldRenderTwiceAlways "OfxImageEffectPluginPropFieldRenderTwiceAlways"
#define kOfxImageEffectPropProjectSize "OfxImageEffectPropProjectSize"
#define kOfxImageEffectPropProjectOffset "OfxImageEffectPropProjectOffset"
#define kOfxImageEffectPropProjectExtent "OfxImageEffectPropProjectExtent"
#define kOfxImageEffectPropProjectPixelAspectRatio "OfxImageEffectPropProjectPixelAspectRatio"
#define kOfxImageEffectInstancePropEffectDuration "OfxImageEffectInstancePropEffectDuration"
#define kOfxImageEffectPropFrameRate "OfxImageEffectPropFrameRate"
#define kOfxImageEffectPropFrameRange "OfxImageEffectPropFrameRange"
#define kOfxImageEffectPropFrameStep "OfxImageEffectPropFrameStep"
#define kOfxImageEffectPropUnmappedFrameRange "OfxImageEffectPropUnmappedFrameRange"
#define kOfxImageEffectPropUnmappedFrameRate "OfxImageEffectPropUnmappedFrameRate"
#define kOfxImageEffectPropRenderWindow "OfxImageEffectPropRenderWindow"
#define kOfxImageEffectPropRenderScale "OfxImageEffectPropRenderScale"
#define kOfxImageEffectPropFieldToRender "OfxImageEffectPropFieldToRender"
#define kOfxImageEffectPropSequentialRenderStatus "OfxImageEffectPropSequentialRenderStatus"
#define kOfxImageEffectPropInteractiveRenderStatus "OfxImageEffectPropInteractiveRenderStatus"
#define kOfxImageEffectPropRegionOfDefinition "OfxImageEffectPropRegionOfDefinition"
#define kOfxImageEffectPropPixelDepth "OfxImageEffectPropPixelDepth"
#define kOfxImageEffectPropComponents "OfxImageEffectPropComponents"
#define kOfxImageEffectPropPreMultiplication "OfxImageEffectPropPreMultiplication"
#define kOfxImageEffectFrameVarying "OfxImageEffectFrameVarying"

// Clips and images
#define kOfxImageEffectSimpleSourceClipName "Source"
#define kOfxImageEffectOutputClipName "Output"
#define kOfxImageClipPropConnected "OfxImageClipPropConnected"
#define kOfxImageClipPropOptional "OfxImageClipPropOptional"
#define kOfxImageClipPropIsMask "OfxImageClipPropIsMask"
#define kOfxImageClipPropFieldExtraction "OfxImageClipPropFieldExtraction"
#define kOfxImageClipPropFieldOrder "OfxImageClipPropFieldOrder"
#define kOfxImageClipPropUnmappedComponents "OfxImageClipPropUnmappedComponents"
#define kOfxImageClipPropUnmappedPixelDepth "OfxImageClipPropUnmappedPixelDepth"
#define kOfxImageClipPropContinuousSamples "OfxImageClipPropContinuousSamples"
#define kOfxImagePropData "OfxImagePropData"
#define kOfxImagePropBounds "OfxImagePropBounds"
#define kOfxImagePropRegionOfDefinition "OfxImagePropRegionOfDefinition"
#define kOfxImagePropRowBytes "OfxImagePropRowBytes"
#define kOfxImagePropPixelAspectRatio "OfxImagePropPixelAspectRatio"
#define kOfxImagePropField "OfxImagePropField"
#define kOfxImagePropUniqueIdentifier "OfxImagePropUniqueIdentifier"
#define kOfxImageFieldNone "OfxFieldNone"
#define kOfxImageComponentRGBA "OfxImageComponentRGBA"
#define kOfxImageComponentRGB "OfxImageComponentRGB"
#define kOfxImageComponentAlpha "OfxImageComponentAlpha"
#define kOfxImageComponentNone "OfxImageComponentNone"
#define kOfxBitDepthNone "OfxBitDepthNone"
#define kOfxBitDepthByte "OfxBitDepthByte"
#define kOfxBitDepthShort "OfxBitDepthShort"
#define kOfxBitDepthHalf "OfxBitDepthHalf"
#define kOfxBitDepthFloat "OfxBitDepthFloat"
#define kOfxImageOpaque "OfxImageOpaque"
#define kOfxImagePreMultiplied "OfxImageAlphaPremultiplied"
#define kOfxImageUnPreMultiplied "OfxImageAlphaUnPremultiplied"

// Parameters
#define kOfxParamTypeInteger "OfxParamTypeInteger"
#define kOfxParamTypeDouble "OfxParamTypeDouble"
#define kOfxParamTypeBoolean "OfxParamTypeBoolean"
#define kOfxParamTypeChoice "OfxParamTypeChoice"
#define kOfxParamTypeStrChoice "OfxParamTypeStrChoice"
#define kOfxParamTypeRGBA "OfxParamTypeRGBA"
#define kOfxParamTypeRGB "OfxParamTypeRGB"
#define kOfxParamTypeDouble2D "OfxParamTypeDouble2D"
#define kOfxParamTypeInteger2D "OfxParamTypeInteger2D"
#define kOfxParamTypeDouble3D "OfxParamTypeDouble3D"
#define kOfxParamTypeInteger3D "OfxParamTypeInteger3D"
#define kOfxParamTypeString "OfxParamTypeString"
#define kOfxParamTypeCustom "OfxParamTypeCustom"
#define kOfxParamTypeBytes "OfxParamTypeBytes"
#define kOfxParamTypeGroup "OfxParamTypeGroup"
#define kOfxParamTypePage "OfxParamTypePage"
#define kOfxParamTypePushButton "OfxParamTypePushButton"
#define kOfxParamTypeParametric "OfxParamTypeParametric"
#define kOfxParamPropType "OfxParamPropType"
#define kOfxParamPropDefault "OfxParamPropDefault"
#define kOfxParamPropMin "OfxParamPropMin"
#define kOfxParamPropMax "OfxParamPropMax"
#define kOfxParamPropDisplayMin "OfxParamPropDisplayMin"
#define kOfxParamPropDisplayMax "OfxParamPropDisplayMax"
#define kOfxParamPropIncrement "OfxParamPropIncrement"
#define kOfxParamPropDigits "OfxParamPropDigits"
#define kOfxParamPropChoiceOption "OfxParamPropChoiceOption"
#define kOfxParamPropDoubleType "OfxParamPropDoubleType"
#define kOfxParamDoubleTypePlain "OfxParamDoubleTypePlain"
#define kOfxParamDoubleTypeAngle "OfxParamDoubleTypeAngle"
#define kOfxParamDoubleTypeScale "OfxParamDoubleTypeScale"
#define kOfxParamDoubleTypeX "OfxParamDoubleTypeX"
#define kOfxParamDoubleTypeY "OfxParamDoubleTypeY"
#define kOfxParamDoubleTypeXAbsolute "OfxParamDoubleTypeXAbsolute"
#define kOfxParamDoubleTypeYAbsolute "OfxParamDoubleTypeYAbsolute"
#define kOfxParamDoubleTypeXY "OfxParamDoubleTypeXY"
#define kOfxParamDoubleTypeXYAbsolute "OfxParamDoubleTypeXYAbsolute"
#define kOfxParamDoubleTypeNormalisedX "OfxParamDoubleTypeNormalisedX"
#define kOfxParamDoubleTypeNormalisedY "OfxParamDoubleTypeNormalisedY"
#define kOfxParamDoubleTypeNormalisedXAbsolute "OfxParamDoubleTypeNormalisedXAbsolute"
#define kOfxParamDoubleTypeNormalisedYAbsolute "OfxParamDoubleTypeNormalisedYAbsolute"
#define kOfxParamDoubleTypeNormalisedXY "OfxParamDoubleTypeNormalisedXY"
#define kOfxParamDoubleTypeNormalisedXYAbsolute "OfxParamDoubleTypeNormalisedXYAbsolute"
#define kOfxParamPropDefaultCoordinateSystem "OfxParamPropDefaultCoordinateSystem"
#define kOfxParamCoordinatesCanonical "OfxParamCoordinatesCanonical"
#define kOfxParamCoordinatesNormalised "OfxParamCoordinatesNormalised"
#define kOfxParamPropAnimates "OfxParamPropAnimates"
#define kOfxParamPropIsAnimating "OfxParamPropIsAnimating"
#define kOfxParamPropSecret "OfxParamPropSecret"
#define kOfxParamPropEnabled "OfxParamPropEnabled"
#define kOfxParamPropHint "OfxParamPropHint"
#define kOfxParamPropScriptName "OfxParamPropScriptName"
#define kOfxParamPropParent "OfxParamPropParent"
#define kOfxParamPropStringMode "OfxParamPropStringMode"
#define kOfxParamStringIsSingleLine "OfxParamStringIsSingleLine"
#define kOfxParamStringIsMultiLine "OfxParamStringIsMultiLine"
#define kOfxParamStringIsFilePath "OfxParamStringIsFilePath"
#define kOfxParamStringIsDirectoryPath "OfxParamStringIsDirectoryPath"
#define kOfxParamStringIsLabel "OfxParamStringIsLabel"
#define kOfxParamPropStringFilePathExists "OfxParamPropStringFilePathExists"
#define kOfxParamPropDimensionLabel "OfxParamPropDimensionLabel"
#define kOfxParamPropPersistant "OfxParamPropPersistant"
#define kOfxParamPropEvaluateOnChange "OfxParamPropEvaluateOnChange"
#define kOfxParamPropCanUndo "OfxParamPropCanUndo"
#define kOfxParamPropCacheInvalidation "OfxParamPropCacheInvalidation"
#define kOfxParamInvalidateValueChange "OfxParamInvalidateValueChange"
#define kOfxParamPropDataPtr "OfxParamPropDataPtr"
#define kOfxParamPropGroupOpen "OfxParamPropGroupOpen"
#define kOfxParamPropPageChild "OfxParamPropPageChild"
#define kOfxParamPropHasHostOverlayHandle "OfxParamPropHasHostOverlayHandle"
#define kOfxParamPropUseHostOverlayHandle "OfxParamPropUseHostOverlayHandle"
#define kOfxParamPropInteractV1 "OfxParamPropInteractV1"
#define kOfxParamPropInteractSize "OfxParamPropInteractSize"
#define kOfxParamPropInteractSizeAspect "OfxParamPropInteractSizeAspect"
#define kOfxParamPropInteractMinimumSize "OfxParamPropInteractMinimumSize"
#define kOfxParamPropInteractPreferedSize "OfxParamPropInteractPreferedSize"
#define kOfxParamPropPluginMayWrite "OfxParamPropPluginMayWrite"
#define kOfxImageEffectPluginPropOverlayInteractV1 "OfxImageEffectPluginPropOverlayInteractV1"

// Message types
#define kOfxMessageFatal "OfxMessageFatal"
#define kOfxMessageError "OfxMessageError"
#define kOfxMessageWarning "OfxMessageWarning"
#define kOfxMessageMessage "OfxMessageMessage"
#define kOfxMessageLog "OfxMessageLog"
#define kOfxMessageQuestion "OfxMessageQuestion"
