// MIT License
//
// Copyright(c) 2021-2022 Matthieu Bucchianeri
// Copyright(c) 2021-2022 Jean-Luc Dupiot - Reality XP
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "pch.h"

#include "factories.h"
#include "interfaces.h"
#include "layer.h"
#include "log.h"
#include "proxy_usage_diagnostic.h"
#include "proxy_scope_diagnostic.h"
#include "proxy_index_diagnostic.h"

namespace {

    using namespace toolkit;
    using namespace toolkit::log;
    using namespace xr::math;
    using namespace toolkit::math;

    // The xrWaitFrame() loop might cause to have 2 frames in-flight, so we want to delay the GPU timer re-use by those
    // 2 frames.
    constexpr uint32_t GpuTimerLatency = 2;
    constexpr uint32_t MetroGfxLogSamples = 8;

    // Keep this transformation identical to the simple-FOV path in xrLocateViews().
    XrFovf ScaleSimpleFov(XrFovf fov, int percent) {
        if (percent != 100) {
            StoreXrFov(&fov, DirectX::XMVectorMultiply(LoadXrFov(fov),
                                                       DirectX::XMVectorReplicate(percent * 0.01f)));
        }
        return fov;
    }

    bool GetFovRatios(const XrFovf& original, const XrFovf& modified, float& width, float& height) {
        constexpr float Limit = 1.57079632679f - 0.001f;
        const float angles[] = {original.angleLeft, original.angleRight, original.angleUp, original.angleDown,
                                modified.angleLeft, modified.angleRight, modified.angleUp, modified.angleDown};
        for (const float angle : angles) {
            if (!std::isfinite(angle) || std::abs(angle) >= Limit) {
                return false;
            }
        }
        if (original.angleLeft >= original.angleRight || original.angleDown >= original.angleUp ||
            modified.angleLeft >= modified.angleRight || modified.angleDown >= modified.angleUp) {
            return false;
        }
        const float originalWidth = std::tan(original.angleRight) - std::tan(original.angleLeft);
        const float originalHeight = std::tan(original.angleUp) - std::tan(original.angleDown);
        width = (std::tan(modified.angleRight) - std::tan(modified.angleLeft)) / originalWidth;
        height = (std::tan(modified.angleUp) - std::tan(modified.angleDown)) / originalHeight;
        return std::isfinite(width) && std::isfinite(height) && width > 0.f && height > 0.f &&
               width <= 1.0001f && height <= 1.0001f;
    }

    uint32_t CropRecommendedSize(uint32_t raw, float ratio) {
        return std::min(raw, roundUp(std::max(2u, static_cast<uint32_t>(std::lround(raw * ratio))), 2u));
    }

    std::filesystem::path GetFovCalibrationPath(const std::string& identity) {
        uint64_t hash = 14695981039346656037ull;
        for (const unsigned char byte : identity) {
            hash = (hash ^ byte) * 1099511628211ull;
        }
        return localAppData / "configs" / fmt::format("fov_crop_calibration_{:016x}.txt", hash);
    }

    bool ReadFovCalibration(const std::filesystem::path& path,
                            const std::string& identity,
                            XrFovf (&fov)[utilities::ViewCount]) {
        std::error_code fileError;
        const auto fileSize = std::filesystem::file_size(path, fileError);
        if (fileError || fileSize > 4096) {
            return false;
        }
        std::ifstream input(path);
        std::string version;
        std::string storedIdentity;
        if (!std::getline(input, version) || version != "FOV_CROP_V2" ||
            !(input >> std::quoted(storedIdentity)) || storedIdentity != identity) {
            return false;
        }
        for (auto& eye : fov) {
            if (!(input >> eye.angleLeft >> eye.angleRight >> eye.angleUp >> eye.angleDown)) {
                return false;
            }
            float width = 0.f, height = 0.f;
            if (!GetFovRatios(eye, eye, width, height)) {
                return false;
            }
        }
        return true;
    }

    bool WriteFovCalibration(const std::filesystem::path& path,
                             const std::string& identity,
                             const XrFovf (&fov)[utilities::ViewCount]) {
        auto temporary = path;
        temporary += fmt::format(".{}.tmp", GetCurrentProcessId());
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) {
                return false;
            }
            output << "FOV_CROP_V2\n" << std::quoted(identity) << '\n' << std::setprecision(9);
            for (const auto& eye : fov) {
                output << eye.angleLeft << ' ' << eye.angleRight << ' ' << eye.angleUp << ' ' << eye.angleDown << '\n';
            }
            output.flush();
            if (!output) {
                return false;
            }
        }
        return MoveFileExW(temporary.c_str(), path.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }

    struct SwapchainImages {
        std::shared_ptr<graphics::ITexture> appTexture;
        std::shared_ptr<graphics::ITexture> runtimeTexture;
        std::shared_ptr<graphics::IGpuTimer> upscalingTimers[utilities::ViewCount];
        std::shared_ptr<graphics::IGpuTimer> postProcessingTimers[utilities::ViewCount];
    };

    struct SwapchainState {
        std::vector<SwapchainImages> images;
        uint32_t acquiredImageIndex{0};
        uint32_t requestedWidth{0};
        uint32_t requestedHeight{0};
        uint32_t requestedArraySize{0};
        bool cropRecommendationLogged[utilities::ViewCount]{false, false};
        bool delayedRelease{false};

        // Intermediate textures for processing.
        std::shared_ptr<graphics::ITexture> nonVPRTInputTexture;
        std::shared_ptr<graphics::ITexture> nonVPRTOutputTexture;
        std::shared_ptr<graphics::ITexture> upscaledTexture;

        // Intermediate textures than can be used for state in the image processors.
        std::vector<std::shared_ptr<graphics::ITexture>> upscalerTextures;
        std::vector<std::shared_ptr<graphics::ITexture>> postProcessorTextures;

        // Opaque blobs of memory that can be used for constant buffer bouncing in the image processors.
        std::array<uint8_t, 1024> upscalerBlob;
        std::array<uint8_t, 1024> postProcessorBlob;

        bool registeredWithFrameAnalyzer{false};
    };

    struct ProxyUsageState {
        XrSession session{XR_NULL_HANDLE};
        uint64_t id{0}, generation{0};
        bool hasProxy{false}, strict{false};
        std::string reason;
        uint32_t roles{0}, lifecycleLogs{0};
        uint64_t firstProjection{0}, firstOther{0}, acquires{0}, releases{0};
        std::set<std::string> roleBindings;
    };

    class OpenXrLayer : public toolkit::OpenXrApi {
      public:
        OpenXrLayer() = default;

        void setOptionsDefaults() {
            m_configManager->setDefault("key_menu_gen", 1);
            m_configManager->setDefault(config::SettingFirstRun, 0);
            m_configManager->setDefault(config::SettingDeveloper, 0);

            // Input & menu options.
            m_configManager->setDefault(config::SettingKeyCtrlModifier, 1);
            m_configManager->setDefault(config::SettingKeyAltModifier, 0);
            m_configManager->setDefault(config::SettingMenuKeyLeft, VK_F1);
            m_configManager->setDefault(config::SettingMenuKeyRight, VK_F3);
            m_configManager->setDefault(config::SettingMenuKeyDown, VK_F2);
            m_configManager->setDefault(config::SettingMenuKeyUp, 0);
            m_configManager->setDefault(config::SettingScreenshotKey, VK_F12);
            m_configManager->setDefault(config::SettingMenuEyeVisibility, 0); // Both
            m_configManager->setDefault(config::SettingMenuEyeOffset, 0);
            m_configManager->setDefault(config::SettingMenuDistance, 100); // 1m
            m_configManager->setDefault(config::SettingMenuOpacity, 85);
            m_configManager->setDefault(config::SettingMenuFontSize, 44); // pt
            m_configManager->setEnumDefault(config::SettingMenuTimeout, config::MenuTimeout::Medium);
            m_configManager->setDefault(config::SettingMenuExpert, m_configManager->isDeveloper());
            m_configManager->setDefault(config::SettingMenuLegacyMode, 0);
            m_configManager->setEnumDefault(config::SettingOverlayType, config::OverlayType::None);
            m_configManager->setDefault(config::SettingOverlayShowClock, 0);
            // Legacy setting is 1/3rd from top and 2/3rd from left.
            {
                const auto ndcOffset = utilities::ScreenToNdc({2 / 3.f, 1 / 3.f});
                m_configManager->setDefault(config::SettingOverlayXOffset, (int)(ndcOffset.x * 100.f));
                m_configManager->setDefault(config::SettingOverlayYOffset, (int)(ndcOffset.y * 100.f));
            }

            // Hand tracking feature.
            m_configManager->setEnumDefault(config::SettingHandTrackingEnabled, config::HandTrackingEnabled::Off);
            m_configManager->setDefault(config::SettingBypassMsftHandInteractionCheck, 0);
            m_configManager->setDefault(config::SettingHandVisibilityAndSkinTone, 2); // Visible - Medium
            m_configManager->setDefault(config::SettingHandOcclusion, 0);
            m_configManager->setDefault(config::SettingHandTimeout, 1);

            // Eye tracking feature.
            m_configManager->setDefault(config::SettingEyeTrackingEnabled, 0);
            m_configManager->setDefault(config::SettingBypassMsftEyeGazeInteractionCheck, 0);
            m_configManager->setDefault(config::SettingEyeDebugWithController, 0);
            m_configManager->setDefault(config::SettingEyeProjectionDistance, 200); // 2m
            m_configManager->setDefault(config::SettingEyeDebug, 0);

            // Upscaling feature.
            m_configManager->setEnumDefault(config::SettingScalingType, config::ScalingType::None);
            m_configManager->setDefault(config::SettingScaling, 100);
            m_configManager->setDefault(config::SettingAnamorphic, -100);
            m_configManager->setDefault(config::SettingSharpness, 20);
            // We default mip-map biasing to Off with OpenComposite since it's causing issues with certain apps. Users
            // have the (Expert) option to turn it back on.
            m_configManager->setEnumDefault(config::SettingMipMapBias,
                                            !m_isOpenComposite ? config::MipMapBias::Anisotropic
                                                               : config::MipMapBias::Off);

            // Foveated rendering.
            m_configManager->setEnumDefault(config::SettingVRS, config::VariableShadingRateType::None);
            m_configManager->setEnumDefault(config::SettingVRSQuality, config::VariableShadingRateQuality::Performance);
            m_configManager->setEnumDefault(config::SettingVRSPattern, config::VariableShadingRatePattern::Wide);
            m_configManager->setDefault(config::SettingVRSInner, 0); // 1x
            m_configManager->setDefault(config::SettingVRSInnerRadius, 55);
            m_configManager->setDefault(config::SettingVRSMiddle, 2); // 1/4x
            m_configManager->setDefault(config::SettingVRSOuter, 4);  // 1/16x
            m_configManager->setDefault(config::SettingVRSOuterRadius, 80);
            m_configManager->setDefault(config::SettingVRSXOffset, 0);
            m_configManager->setDefault(config::SettingVRSXScale, 125);
            m_configManager->setDefault(config::SettingVRSYOffset, 0);
            m_configManager->setDefault(config::SettingVRSPreferHorizontal, 0);
            m_configManager->setDefault(config::SettingVRSLeftRightBias, 0);
            // Fix issue in iRacing: https://forums.iracing.com/discussion/comment/310749
            m_configManager->setDefault(config::SettingVRSScaleFilter,
                                        m_applicationName != "iRacingSim64DX11" ? 80 : 90);
            m_configManager->setDefault(config::SettingVRSCullHAM, 0);

            // Appearance.
            m_configManager->setDefault(config::SettingPostProcess, 0);
            m_configManager->setDefault(config::SettingPostSunGlasses, 0);
            m_configManager->setDefault(config::SettingPostContrast, 500);
            m_configManager->setDefault(config::SettingPostBrightness, 500);
            m_configManager->setDefault(config::SettingPostExposure, 500);
            m_configManager->setDefault(config::SettingPostSaturation, 500);
            m_configManager->setDefault(config::SettingPostColorGainR, 500);
            m_configManager->setDefault(config::SettingPostColorGainG, 500);
            m_configManager->setDefault(config::SettingPostColorGainB, 500);
            m_configManager->setDefault(config::SettingPostVibrance, 0);
            m_configManager->setDefault(config::SettingPostHighlights, 1000);
            m_configManager->setDefault(config::SettingPostShadows, 0);
            m_configManager->setDefault(config::SettingPostChromaticCorrectionR, 100090);
            m_configManager->setDefault(config::SettingPostChromaticCorrectionB, 99880);

            // TODO: Appearance (User)
#if 0
            m_configManager->setDefault(config::SettingPostContrast + "_u1", 500);
            m_configManager->setDefault(config::SettingPostBrightness + "_u1", 500);
            m_configManager->setDefault(config::SettingPostExposure + "_u1", 500);
            m_configManager->setDefault(config::SettingPostSaturation + "_u1", 500);
            m_configManager->setDefault(config::SettingPostColorGainR + "_u1", 500);
            m_configManager->setDefault(config::SettingPostColorGainG + "_u1", 500);
            m_configManager->setDefault(config::SettingPostColorGainB + "_u1", 500);
            m_configManager->setDefault(config::SettingPostVibrance + "_u1", 0);
            m_configManager->setDefault(config::SettingPostHighlights + "_u1", 1000);
            m_configManager->setDefault(config::SettingPostShadows + "_u1", 0);
#endif
            // Misc features.
            m_configManager->setDefault(config::SettingICD, 1000);
            m_configManager->setDefault(config::SettingFOVType, 0); // Simple
            m_configManager->setDefault(config::SettingFOV, 100);
            m_configManager->setDefault(config::SettingCropResolutionToFOV, 0);
            m_configManager->setDefault(config::SettingFOVUp, 100);
            m_configManager->setDefault(config::SettingFOVDown, 100);
            m_configManager->setDefault(config::SettingFOVLeftLeft, 100);
            m_configManager->setDefault(config::SettingFOVLeftRight, 100);
            m_configManager->setDefault(config::SettingFOVRightLeft, 100);
            m_configManager->setDefault(config::SettingFOVRightRight, 100);
            m_configManager->setDefault(config::SettingZoom, 10);
            m_configManager->setDefault(config::SettingDisableHAM, 0);
            m_configManager->setEnumDefault(config::SettingBlindEye, config::BlindEye::None);
            m_configManager->setDefault(config::SettingPredictionDampen, 100);
            m_configManager->setDefault(config::SettingResolutionOverride, 0);
            m_configManager->setEnumDefault(config::SettingMotionReprojection, config::MotionReprojection::Default);
            m_configManager->setEnumDefault(config::SettingMotionReprojectionRate, config::MotionReprojectionRate::Off);
            m_configManager->setEnumDefault(config::SettingScreenshotFileFormat, config::ScreenshotFileFormat::PNG);
            m_configManager->setDefault(config::SettingScreenshotEye, 0); // Both
            m_configManager->setDefault(config::SettingRecordStats, 0);
            m_configManager->setDefault(config::SettingHighRateStats, 0);
            m_configManager->setDefault(config::SettingFrameThrottling, config::MaxFrameRate); // Off
            m_configManager->setDefault(config::SettingTargetFrameRate, config::MaxFrameRate); // Off
            m_configManager->setDefault(config::SettingTargetFrameRate2, 0);

            // We disable the API interceptor with certain games where it seems to cause issues. As a result, foveated
            // rendering will not be offered.
            m_configManager->setDefault(config::SettingDisableInterceptor,
                                        (m_applicationName == "re2" || m_applicationName == "OpenComposite_Il-2") ? 1
                                                                                                                  : 0);
            // We disable the frame analyzer when using OpenComposite, because the app does not see the OpenXR
            // textures anyways.
            m_configManager->setDefault("disable_frame_analyzer",
                                        m_isOpenComposite || m_applicationName == "DCS World");
            m_configManager->setDefault("canting", 0);
            m_configManager->setDefault("vrs_capture", 0);
            m_configManager->setDefault("force_vprt_path", 0);
            m_configManager->setDefault("droolon_port", 5347);
            m_configManager->setDefault("allow_ca_correction", 0);

            // Workaround: the first versions of the toolkit used a different representation for the world scale.
            // Migrate the value upon first run.
            m_configManager->setDefault("icd", 0);
            if (m_configManager->getValue("icd") != 0) {
                const int migratedValue = 1'000'000 / m_configManager->getValue("icd");
                m_configManager->setValue(config::SettingICD, migratedValue, true);
                m_configManager->deleteValue("icd");
            }

            // Commit any update above. This is needed for apps that create an instance, destroy it right away
            // without submitting a frame, then create a new one.
            m_configManager->tick();
        }

        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(xr::ToString(createInfo->applicationInfo.apiVersion).c_str(), "ApiVersion"),
                              TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"),
                              TLArg(createInfo->applicationInfo.applicationVersion, "ApplicationVersion"),
                              TLArg(createInfo->applicationInfo.engineName, "EngineName"),
                              TLArg(createInfo->applicationInfo.engineVersion, "EngineVersion"),
                              TLArg(createInfo->createFlags, "CreateFlags"));

            // Needed to resolve the requested function pointers.
            OpenXrApi::xrCreateInstance(createInfo);

            {
                PFN_xrVoidFunction unused;
                m_hasPerformanceCounterKHR = XR_SUCCEEDED(
                    xrGetInstanceProcAddr(GetXrInstance(), "xrConvertWin32PerformanceCounterToTimeKHR", &unused));
            }
            {
                PFN_xrVoidFunction unused;
                m_hasVisibilityMaskKHR =
                    XR_SUCCEEDED(xrGetInstanceProcAddr(GetXrInstance(), "xrGetVisibilityMaskKHR", &unused));
            }
            bool hasEyeTrackerFB = false;
            {
                PFN_xrVoidFunction unused;
                hasEyeTrackerFB = XR_SUCCEEDED(xrGetInstanceProcAddr(GetXrInstance(), "xrCreateEyeTrackerFB", &unused));
            }
            m_applicationName = createInfo->applicationInfo.applicationName;
            if (isGraphicsNeutralApp()) {
                char modeText[8]{};
                const DWORD modeLength = GetEnvironmentVariableA("OXRTK_PROXY_TIMING_TEST", modeText, sizeof(modeText));
                if (modeLength == 1 && modeText[0] >= '0' && modeText[0] <= '2') {
                    m_proxyTimingMode = static_cast<uint32_t>(modeText[0] - '0');
                } else if (modeLength != 0) {
                    Log("[PROXY-TIMING] app=%s invalid OXRTK_PROXY_TIMING_TEST; using mode=0\n",
                        m_applicationName.c_str());
                }
                // All modes retain Submode 2's private color proxy and raw CopyResource path.
                // Synchronization is pinned to Proxy Sync Mode 2 so allocator lifetime and current-copy
                // completion are identical in every timing mode. Only copy/release timing is varied.
                m_proxyTimingEnabled = true;
                m_proxySyncEnabled = true;
                m_proxySyncMode = 2;
                m_gfxSubBisectMode = 2;
                m_gfxBisectMode = 2;
                // Retain prior usage/timing diagnostics; scope below pins their production selection
                // to U0 / timing Mode 0 regardless of the old environment variables.
                char usageText[8]{};
                const DWORD usageLength = GetEnvironmentVariableA("OXRTK_PROXY_USAGE_TEST", usageText,
                                                                   sizeof(usageText));
                m_proxyUsageEnabled = true;
                m_proxyUsageMode = proxy_usage::ParseMode(usageText, usageLength);
                m_proxyTimingMode = 0;
                m_proxyUsageAppIdentity = fmt::format("{}:{}:{}:{}", m_applicationName,
                    createInfo->applicationInfo.applicationVersion, createInfo->applicationInfo.engineName,
                    createInfo->applicationInfo.engineVersion);
                char scopeText[8]{};
                const auto scopeLength = GetEnvironmentVariableA("OXRTK_PROXY_SCOPE_TEST", scopeText, sizeof(scopeText));
                m_proxyScopeEnabled = true;
                m_proxyScopeMode = proxy_scope::ParseMode(scopeText, scopeLength);
                char indexText[8]{};
                const DWORD indexLength = GetEnvironmentVariableA("OXRTK_PROXY_INDEX_TEST", indexText, sizeof(indexText));
                m_proxyIndexMode = indexLength == 1 && indexText[0] == '1' ? 1u : 0u;
                m_proxyIndexEnabled = m_proxyScopeMode == 1;
                Log("[PROXY-INDEX] app=%s mode=%u active=%u scope_mode=%u reason=%s\n",
                    m_applicationName.c_str(), m_proxyIndexMode, m_proxyIndexEnabled, m_proxyScopeMode,
                    m_proxyIndexEnabled ? "projection_proxy_only" : "requires_OXRTK_PROXY_SCOPE_TEST_1");
                m_proxyUsageMode = 0; // Usage/timing/sync/graphics environment variables cannot vary scope.
                m_proxyTimingMode = 0;
                Log("[PROXY-SCOPE] app=%s mode=%u usage=original copy_point=release "
                    "runtime_release_point=release allocator_safe=1 wait_current_copy=1 old_envs=ignored\n",
                    m_applicationName.c_str(), m_proxyScopeMode);
                Log("[PROXY-USAGE] startup app=%s mode=%u original_usage=per_swapchain "
                    "downstream_transfer_dst=per_swapchain transfer_dst_test=%u copy_point=release runtime_release_point=release "
                    "allocator_safe=1 wait_current_copy=1 old_timing_env=ignored\n",
                    m_applicationName.c_str(), m_proxyUsageMode, m_proxyUsageMode);
            }
            Log("Application name: '%s', Engine name: '%s'\n",
                createInfo->applicationInfo.applicationName,
                createInfo->applicationInfo.engineName);
            m_isOpenComposite = m_applicationName.find("OpenComposite_") == 0;
            if (m_isOpenComposite) {
                Log("Detected OpenComposite\n");
            }
            m_isUnity = std::string_view(createInfo->applicationInfo.engineName) == "Unity";

            // Dump the OpenXR runtime information to help debugging customer issues.
            XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES, nullptr};
            CHECK_XRCMD(xrGetInstanceProperties(GetXrInstance(), &instanceProperties));
            TraceLoggingWrite(g_traceProvider,
                              "xrGetInstanceProperties",
                              TLArg(instanceProperties.runtimeName, "RuntimeName"),
                              TLArg(xr::ToString(instanceProperties.runtimeVersion).c_str(), "RuntimeVersion"));
            m_runtimeName = fmt::format("{} {}.{}.{}",
                                        instanceProperties.runtimeName,
                                        XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                        XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                        XR_VERSION_PATCH(instanceProperties.runtimeVersion));
            Log("Using OpenXR runtime %s\n", m_runtimeName.c_str());
            DiagnosticLog("instance app=%s opencomposite=%u runtime=%s enabled_extensions=%u",
                          m_applicationName.c_str(), m_isOpenComposite, m_runtimeName.c_str(),
                          createInfo->enabledExtensionCount);

            m_configManager = config::CreateConfigManager(createInfo->applicationInfo.applicationName);
            setOptionsDefaults();

            if (m_configManager->isDeveloper()) {
                TraceLoggingWrite(g_traceProvider, "DeveloperMode");
                Log("DEVELOPER MODE IS ENABLED! WARRANTY IS VOID!\n");
            }
            if (m_configManager->isSafeMode()) {
                TraceLoggingWrite(g_traceProvider, "SafeMode");
                Log("SAFE MODE IS ENABLED! NO SETTINGS ARE LOADED!\n");
            }

            // Check what keys to use.
            if (m_configManager->getValue(config::SettingKeyCtrlModifier)) {
                m_keyModifiers.push_back(VK_CONTROL);
            }
            if (m_configManager->getValue(config::SettingKeyAltModifier)) {
                m_keyModifiers.push_back(VK_MENU);
            }
            m_keyScreenshot = m_configManager->getValue(config::SettingScreenshotKey);

            // We must initialize hand and eye tracking early on, because the application can start creating actions etc
            // before creating the session.
            if (m_configManager->getEnumValue<config::HandTrackingEnabled>(config::SettingHandTrackingEnabled) !=
                config::HandTrackingEnabled::Off) {
                m_handTracker = input::CreateHandTracker(*this, m_configManager);
                m_sendInterationProfileEvent = true;
            }

            // For eye tracking, we try to use the Omnicept runtime if it's available.
            std::unique_ptr<HP::Omnicept::Client> omniceptClient;
            if (utilities::IsServiceRunning("HP Omnicept")) {
                try {
                    HP::Omnicept::Client::StateCallback_T stateCallback = [&](const HP::Omnicept::Client::State state) {
                        if (state == HP::Omnicept::Client::State::RUNNING ||
                            state == HP::Omnicept::Client::State::PAUSED) {
                            Log("Omnicept client connected\n");
                        } else if (state == HP::Omnicept::Client::State::DISCONNECTED) {
                            Log("Omnicept client disconnected\n");
                        }
                    };

                    std::unique_ptr<HP::Omnicept::Glia::AsyncClientBuilder> omniceptClientBuilder =
                        HP::Omnicept::Glia::StartBuildClient_Async(
                            "OpenXR-Toolkit",
                            std::move(std::make_unique<HP::Omnicept::Abi::SessionLicense>(
                                "", "", HP::Omnicept::Abi::LicensingModel::CORE, false)),
                            stateCallback);

                    omniceptClient = std::move(omniceptClientBuilder->getBuildClientResultOrThrow());
                    Log("Detected HP Omnicept support\n");
                    m_isOmniceptDetected = true;
                } catch (const HP::Omnicept::Abi::HandshakeError& e) {
                    Log("Could not connect to Omnicept runtime HandshakeError: %s\n", e.what());
                } catch (const HP::Omnicept::Abi::TransportError& e) {
                    Log("Could not connect to Omnicept runtime TransportError: %s\n", e.what());
                } catch (const HP::Omnicept::Abi::ProtocolError& e) {
                    Log("Could not connect to Omnicept runtime ProtocolError: %s\n", e.what());
                } catch (std::exception& e) {
                    Log("Could not connect to Omnicept runtime: %s\n", e.what());
                }
            }

            // ...and the Pimax eye tracker if available.
            {
                XrSystemGetInfo getInfo{XR_TYPE_SYSTEM_GET_INFO};
                getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
                XrSystemId systemId;
                if (XR_SUCCEEDED(OpenXrApi::xrGetSystem(GetXrInstance(), &getInfo, &systemId))) {
                    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
                    CHECK_XRCMD(OpenXrApi::xrGetSystemProperties(GetXrInstance(), systemId, &systemProperties));
                    if (std::string(systemProperties.systemName).find("aapvr") != std::string::npos) {
                        aSeeVRInitParam param;
                        param.ports[0] = m_configManager->getValue("droolon_port");
                        Log("--> aSeeVR_connect_server(%d)\n", param.ports[0]);
                        const auto code = aSeeVR_connect_server(&param);
                        m_hasPimaxEyeTracker = code == ASEEVR_RETURN_CODE::success;
                        Log("<-- aSeeVR_connect_server %d\n", code);
                        if (m_hasPimaxEyeTracker) {
                            Log("Detected Pimax Droolon support\n");
                        }
                    }
                }
            }

            // ...otherwise, we will try to fallback to OpenXR.

            // TODO: If Foveated Rendering is disabled, maybe do not initialize the eye tracker?
            const bool eyeTrackingConfigured = m_configManager->getValue(config::SettingEyeTrackingEnabled) != 0;
            const char* eyeTrackerProvider = "none";
            if (eyeTrackingConfigured) {
                if (omniceptClient) {
                    m_eyeTracker = input::CreateOmniceptEyeTracker(*this, m_configManager, std::move(omniceptClient));
                    eyeTrackerProvider = "omnicept";
                } else if (m_hasPimaxEyeTracker) {
                    m_eyeTracker = input::CreatePimaxEyeTracker(*this, m_configManager);
                    eyeTrackerProvider = "pimax";
                } else if (hasEyeTrackerFB) {
                    m_eyeTracker = input::CreateEyeTrackerFB(*this, m_configManager);
                    eyeTrackerProvider = "openxr_fb";
                } else {
                    m_eyeTracker = input::CreateEyeTracker(*this, m_configManager);
                    eyeTrackerProvider = "openxr_ext";

                    m_needVarjoPollEventWorkaround = m_runtimeName.find("Varjo") != std::string::npos;
                }
            }
            DiagnosticLog("eye tracker object created=%u provider=%s setting_enabled=%u",
                          !!m_eyeTracker, eyeTrackerProvider, eyeTrackingConfigured);

            // Clear HAM-related events so they don't fire off unnecessarily.
            (void)m_configManager->getValue(config::SettingDisableHAM);
            (void)m_configManager->getEnumValue<config::BlindEye>(config::SettingBlindEye);

            // We want to log a warning if HAGS is on.
            const auto hwSchMode = utilities::RegGetDword(
                HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode");
            if (hwSchMode && hwSchMode.value() == 2) {
                Log("HAGS is on\n");
            }

            return XR_SUCCESS;
        }

        ~OpenXrLayer() override {
            if (m_configManager) {
                m_configManager->setActiveSession("");
            }

            utilities::RestoreTimerPrecision();
        }

        XrResult xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) override {
            if (getInfo->type != XR_TYPE_SYSTEM_GET_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrGetSystem",
                              TLPArg(instance, "Instance"),
                              TLArg(xr::ToCString(getInfo->formFactor), "FormFactor"));

            const XrResult result = OpenXrApi::xrGetSystem(instance, getInfo, systemId);
            if (XR_SUCCEEDED(result) && getInfo->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY &&
                m_vrSystemId == XR_NULL_SYSTEM_ID) {
                // Retrieve the actual OpenXR resolution.
                XrViewConfigurationView views[utilities::ViewCount] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW, nullptr},
                                                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW, nullptr}};
                uint32_t viewCount;
                CHECK_XRCMD(OpenXrApi::xrEnumerateViewConfigurationViews(instance,
                                                                         *systemId,
                                                                         XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                         utilities::ViewCount,
                                                                         &viewCount,
                                                                         views));

                m_cropActive = false;
                m_cropExact = false;
                m_cropCacheHit = false;
                m_cropCalibrationIdentity.clear();
                m_cropCalibrationPath.clear();
                m_cropEnumerationLogged = false;
                for (uint32_t eye = 0; eye < std::min(viewCount, utilities::ViewCount); eye++) {
                    m_runtimeRecommendedWidth[eye] = views[eye].recommendedImageRectWidth;
                    m_runtimeRecommendedHeight[eye] = views[eye].recommendedImageRectHeight;
                    m_cropRecommendedWidth[eye] = m_runtimeRecommendedWidth[eye];
                    m_cropRecommendedHeight[eye] = m_runtimeRecommendedHeight[eye];
                    Log("[FOV-CROP] runtime eye=%u recommended=%ux%u max=%ux%u\n",
                        eye, views[eye].recommendedImageRectWidth, views[eye].recommendedImageRectHeight,
                        views[eye].maxImageRectWidth, views[eye].maxImageRectHeight);
                }

                m_displayWidth = views[0].recommendedImageRectWidth;
                m_displayHeight = views[0].recommendedImageRectHeight;

                m_resolutionHeightRatio = (float)m_displayHeight / m_displayWidth;

                m_configManager->setDefault(config::SettingResolutionHeight, m_displayHeight);

                // Workaround: the previous versions of the toolkit used a different representation for the resolution
                // override.
                // Migrate the value upon first run.
                m_configManager->setDefault("resolution_width", 0);
                if (m_configManager->getValue("resolution_width") != 0) {
                    const int migratedValue =
                        (int)(m_configManager->getValue("resolution_width") * m_resolutionHeightRatio);
                    m_configManager->setValue(config::SettingResolutionHeight, migratedValue, true);
                    m_configManager->deleteValue("resolution_width");
                    m_configManager->tick();
                }

                m_maxDisplayHeight = std::min(views[0].maxImageRectHeight,
                                              (uint32_t)(views[0].maxImageRectWidth * m_resolutionHeightRatio));

                // Check for hand and eye tracking support.
                XrSystemHandTrackingPropertiesEXT handTrackingSystemProperties{
                    XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT, nullptr};
                handTrackingSystemProperties.supportsHandTracking = false;

                XrSystemEyeGazeInteractionPropertiesEXT eyeTrackingSystemProperties{
                    XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT, &handTrackingSystemProperties};
                eyeTrackingSystemProperties.supportsEyeGazeInteraction = false;

                XrSystemEyeTrackingPropertiesFB eyeTrackingFBSystemProperties{XR_TYPE_SYSTEM_EYE_TRACKING_PROPERTIES_FB,
                                                                              &eyeTrackingSystemProperties};
                eyeTrackingFBSystemProperties.supportsEyeTracking = false;

                XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES, &eyeTrackingFBSystemProperties};
                CHECK_XRCMD(OpenXrApi::xrGetSystemProperties(instance, *systemId, &systemProperties));

                m_systemName = systemProperties.systemName;
                TraceLoggingWrite(
                    g_traceProvider,
                    "xrGetSystem",
                    TLArg(m_systemName.c_str(), "System"),
                    TLArg(fmt::format("{}x{}", m_displayWidth, m_displayHeight).c_str(), "RecommendedResolution"),
                    TLArg(handTrackingSystemProperties.supportsHandTracking, "SupportsHandTracking"),
                    TLArg(eyeTrackingSystemProperties.supportsEyeGazeInteraction, "SupportsEyeGazeInteraction"));
                Log("Using OpenXR system %s\n", m_systemName.c_str());

                const auto isWMR = m_runtimeName.find("Windows Mixed Reality Runtime") != std::string::npos;
                const auto isVive = m_runtimeName.find("Vive Reality Runtime") != std::string::npos;
                const auto isVarjo = m_runtimeName.find("Varjo") != std::string::npos;
                const auto isSteamVR = m_runtimeName.find("SteamVR") != std::string::npos;

                m_supportMotionReprojectionLock = isWMR;

                // Fix Fallout 4 / OpenComposite Decal Issue for WMR
                m_overrideParallelProjection =
                    m_applicationName == "OpenComposite_Fallout4VR" && isWMR;

                // Workaround: the Vive runtime does not seem to properly convert timestamps. We disable any feature
                // depending on timestamps conversion.
                m_hasPerformanceCounterKHR = !isVive;

                // Workaround: the Varjo and SteamVR runtimes always advertises maxImageRect==recommendedImageRect.
                if (isVarjo || isSteamVR) {
                    m_maxDisplayHeight = 8192;
                }

                m_supportHandTracking = handTrackingSystemProperties.supportsHandTracking;
                m_supportEyeTracking = eyeTrackingSystemProperties.supportsEyeGazeInteraction ||
                                       eyeTrackingFBSystemProperties.supportsEyeTracking || m_isOmniceptDetected ||
                                       m_hasPimaxEyeTracker ||
                                       m_configManager->getValue(config::SettingEyeDebugWithController);
                DiagnosticLog("system=%s eye_gaze_system_support=%u eye_support=%u provider_openxr=%u",
                              m_systemName.c_str(), eyeTrackingSystemProperties.supportsEyeGazeInteraction,
                              m_supportEyeTracking, m_eyeTracker && !m_isOmniceptDetected && !m_hasPimaxEyeTracker);
                const bool isEyeTrackingThruRuntime =
                    m_supportEyeTracking && !(m_isOmniceptDetected || m_hasPimaxEyeTracker);

                // Workaround: the WMR runtime supports mapping the VR controllers through XR_EXT_hand_tracking, which
                // will (falsely) advertise hand tracking support. Check for the Ultraleap layer in this case.
                if (m_supportHandTracking &&
                    (!m_configManager->isDeveloper() &&
                     (!m_configManager->getValue(config::SettingBypassMsftHandInteractionCheck) && isWMR))) {
                    bool hasUltraleapLayer = false;
                    for (const auto& layer : GetUpstreamLayers()) {
                        if (layer == "XR_APILAYER_ULTRALEAP_hand_tracking") {
                            hasUltraleapLayer = true;
                        }
                    }
                    if (!hasUltraleapLayer) {
                        Log("Ignoring XR_MSFT_hand_interaction for %s\n", m_runtimeName.c_str());
                        m_supportHandTracking = false;
                    }
                }

                // Workaround: the WMR runtime supports emulating eye tracking for development through
                // XR_EXT_eye_gaze_interaction, which will (falsely) advertise eye tracking support. Disable it.
                if (isEyeTrackingThruRuntime &&
                    (!m_configManager->isDeveloper() &&
                     (!m_configManager->getValue(config::SettingBypassMsftEyeGazeInteractionCheck) && isWMR))) {
                    Log("Ignoring XR_EXT_eye_gaze_interaction for %s\n", m_runtimeName.c_str());
                    m_supportEyeTracking = false;
                }

                // We had to initialize the hand and eye trackers early on. If we find out now that they are not
                // supported, then destroy them. This could happen if the option was set while a hand tracking device
                // was connected, but later the hand tracking device was disconnected.
                if (!m_supportHandTracking) {
                    m_handTracker.reset();
                }
                if (!m_supportEyeTracking) {
                    m_eyeTracker.reset();
                }
                const bool providerOpenXr = m_eyeTracker && !m_isOmniceptDetected && !m_hasPimaxEyeTracker;
                const bool eyeTrackingConfigured = m_configManager->getValue(config::SettingEyeTrackingEnabled) != 0;
                const char* providerReason = providerOpenXr ? "eligible"
                                             : !eyeTrackingConfigured ? "eye_tracking_setting_off"
                                             : !m_supportEyeTracking ? "system_unsupported_or_runtime_filtered"
                                             : m_isOmniceptDetected ? "omnicept_preferred"
                                             : m_hasPimaxEyeTracker ? "pimax_preferred"
                                             : "tracker_not_created";
                DiagnosticLog("eye provider eligibility setting_enabled=%u system_support=%u eye_support=%u provider_openxr=%u tracker_created=%u reason=%s",
                              eyeTrackingConfigured, eyeTrackingSystemProperties.supportsEyeGazeInteraction,
                              m_supportEyeTracking, providerOpenXr, !!m_eyeTracker, providerReason);

                // Apply override to the target resolution.
                if (m_configManager->getValue(config::SettingResolutionOverride)) {
                    m_displayHeight = m_configManager->getValue(config::SettingResolutionHeight);
                    m_displayWidth = (uint32_t)(m_displayHeight / m_resolutionHeightRatio);

                    Log("Overriding OpenXR resolution: %ux%u\n", m_displayWidth, m_displayHeight);
                }

                m_cropFovPercent = m_configManager->peekValue(config::SettingFOV);
                const bool cropRequested = m_configManager->peekValue(config::SettingCropResolutionToFOV);
                if (cropRequested && viewCount == utilities::ViewCount) {
                    m_cropCalibrationIdentity = fmt::format(
                        "runtime={}|system={}|vendor={}|viewConfig={}|viewCount={}|eye0={}x{}|eye1={}x{}",
                        m_runtimeName, m_systemName, systemProperties.vendorId,
                        static_cast<int>(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO), viewCount,
                        m_runtimeRecommendedWidth[0], m_runtimeRecommendedHeight[0],
                        m_runtimeRecommendedWidth[1], m_runtimeRecommendedHeight[1]);
                    m_cropCalibrationPath = GetFovCalibrationPath(m_cropCalibrationIdentity);
                    m_cropCacheHit = ReadFovCalibration(m_cropCalibrationPath, m_cropCalibrationIdentity,
                                                        m_cropOriginalFov);
                    Log("[FOV-CROP] calibration cache %s key=\"%s\" file=\"%s\"\n",
                        m_cropCacheHit ? "hit" : "miss", m_cropCalibrationIdentity.c_str(),
                        m_cropCalibrationPath.string().c_str());
                    if (m_cropCacheHit) {
                        for (uint32_t eye = 0; eye < utilities::ViewCount; eye++) {
                            Log("[FOV-CROP] calibration loaded eye=%u original=%s\n", eye,
                                xr::ToString(m_cropOriginalFov[eye]).c_str());
                        }
                    }
                }
                const char* cropReason = "active";
                if (!cropRequested) {
                    cropReason = "off";
                } else if (viewCount != utilities::ViewCount) {
                    cropReason = "unsupported_view_count";
                } else if (m_runtimeRecommendedWidth[0] < 2 || m_runtimeRecommendedHeight[0] < 2 ||
                           m_runtimeRecommendedWidth[1] < 2 || m_runtimeRecommendedHeight[1] < 2) {
                    cropReason = "invalid_runtime_recommendation";
                } else if (m_configManager->peekEnumValue<config::ScalingType>(config::SettingScalingType) !=
                           config::ScalingType::None) {
                    cropReason = "upscaling_conflict";
                } else if (m_configManager->peekValue(config::SettingResolutionOverride)) {
                    cropReason = "resolution_override_conflict";
                } else if (m_configManager->peekValue(config::SettingFOVType) != 0) {
                    cropReason = "advanced_fov_unsupported";
                } else if (m_cropFovPercent < 50 || m_cropFovPercent > 100) {
                    cropReason = "invalid_fov_percent";
                } else if (m_cropFovPercent == 100) {
                    cropReason = "fov_not_reduced";
                } else {
                    m_cropActive = true;
                    m_cropExact = m_cropCacheHit;
                    float exactWidth[utilities::ViewCount]{};
                    float exactHeight[utilities::ViewCount]{};
                    if (m_cropExact) {
                        for (uint32_t eye = 0; eye < utilities::ViewCount; eye++) {
                            const XrFovf modified = ScaleSimpleFov(m_cropOriginalFov[eye], m_cropFovPercent);
                            if (!GetFovRatios(m_cropOriginalFov[eye], modified,
                                              exactWidth[eye], exactHeight[eye])) {
                                m_cropExact = false;
                                m_cropCacheHit = false;
                                Log("[FOV-CROP] invalid calibration ratio eye=%u; using linear fallback\n", eye);
                                break;
                            }
                        }
                    }
                    m_displayWidth = m_displayHeight = 0;
                    for (uint32_t eye = 0; eye < utilities::ViewCount; eye++) {
                        if (m_cropExact) {
                            m_cropRecommendedWidth[eye] =
                                CropRecommendedSize(m_runtimeRecommendedWidth[eye], exactWidth[eye]);
                            m_cropRecommendedHeight[eye] =
                                CropRecommendedSize(m_runtimeRecommendedHeight[eye], exactHeight[eye]);
                        } else {
                            std::tie(m_cropRecommendedWidth[eye], m_cropRecommendedHeight[eye]) =
                                config::GetScaledDimensions(m_cropFovPercent, -1,
                                                            m_runtimeRecommendedWidth[eye],
                                                            m_runtimeRecommendedHeight[eye], 2);
                            m_cropRecommendedWidth[eye] =
                                std::min(m_cropRecommendedWidth[eye], m_runtimeRecommendedWidth[eye]);
                            m_cropRecommendedHeight[eye] =
                                std::min(m_cropRecommendedHeight[eye], m_runtimeRecommendedHeight[eye]);
                        }
                        m_displayWidth = std::max(m_displayWidth, m_cropRecommendedWidth[eye]);
                        m_displayHeight = std::max(m_displayHeight, m_cropRecommendedHeight[eye]);
                        const float widthRatio = static_cast<float>(m_cropRecommendedWidth[eye]) /
                                                 m_runtimeRecommendedWidth[eye];
                        const float heightRatio = static_cast<float>(m_cropRecommendedHeight[eye]) /
                                                  m_runtimeRecommendedHeight[eye];
                        Log("[FOV-CROP] crop mode=%s eye=%u raw=%ux%u calculated=%ux%u "
                            "exactWidthRatio=%.6f exactHeightRatio=%.6f appliedWidthRatio=%.6f "
                            "appliedHeightRatio=%.6f relativePixels=%.6f\n",
                            m_cropExact ? "exact" : "linear_fallback", eye,
                            m_runtimeRecommendedWidth[eye], m_runtimeRecommendedHeight[eye],
                            m_cropRecommendedWidth[eye], m_cropRecommendedHeight[eye],
                            m_cropExact ? exactWidth[eye] : 0.f,
                            m_cropExact ? exactHeight[eye] : 0.f,
                            widthRatio, heightRatio, widthRatio * heightRatio);
                    }
                    if (!m_cropExact) {
                        Log("[FOV-CROP] exact crop unavailable - calibration pending; "
                            "linear fallback for this execution\n");
                    }
                    Log("[FOV-CROP] shared texture-array policy: max of both eyes=%ux%u; "
                        "per-eye recommendations remain available for separate swapchains\n",
                        m_displayWidth, m_displayHeight);
                }
                const float widthRatio = m_cropActive ? static_cast<float>(m_displayWidth) /
                                                           m_runtimeRecommendedWidth[0]
                                                      : 1.f;
                const float heightRatio = m_cropActive ? static_cast<float>(m_displayHeight) /
                                                            m_runtimeRecommendedHeight[0]
                                                       : 1.f;
                Log("[FOV-CROP] app=%s opencomposite=%u requested=%u active=%u reason=%s fov_type=%d "
                    "scaling_type=%d resolution_override=%u fov_percent=%d mode=%s sharedMax=%ux%u "
                    "widthRatio=%.4f heightRatio=%.4f relativePixels=%.4f\n",
                    m_applicationName.c_str(), m_isOpenComposite, cropRequested, m_cropActive, cropReason,
                    m_configManager->peekValue(config::SettingFOVType),
                    static_cast<int>(m_configManager->peekEnumValue<config::ScalingType>(config::SettingScalingType)),
                    m_configManager->peekValue(config::SettingResolutionOverride), m_cropFovPercent,
                    m_cropActive ? (m_cropExact ? "exact" : "linear_fallback") : "inactive",
                    m_displayWidth, m_displayHeight, widthRatio, heightRatio,
                    widthRatio * heightRatio);

                // Remember the XrSystemId to use.
                m_vrSystemId = *systemId;

                TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg((int)*systemId, "SystemId"));
            }

            return result;
        }

        XrResult xrEnumerateViewConfigurationViews(XrInstance instance,
                                                   XrSystemId systemId,
                                                   XrViewConfigurationType viewConfigurationType,
                                                   uint32_t viewCapacityInput,
                                                   uint32_t* viewCountOutput,
                                                   XrViewConfigurationView* views) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrEnumerateViewConfigurationViews",
                              TLPArg(instance, "Instance"),
                              TLArg((int)systemId, "SystemId"),
                              TLArg(viewCapacityInput, "ViewCapacityInput"),
                              TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"));

            const XrResult result = OpenXrApi::xrEnumerateViewConfigurationViews(
                instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views);
            if (isGraphicsNeutralApp() && m_gfxBisectMode <= 2) {
                if (XR_SUCCEEDED(result) && views && m_metroGfxEnumerationLogs.fetch_add(1) < MetroGfxLogSamples) {
                    for (uint32_t eye = 0; eye < *viewCountOutput; eye++) {
                        Log("[GFX-NEUTRAL] recommended eye=%u runtime=%ux%u delivered=%ux%u "
                            "resolution_override=0\n", eye, views[eye].recommendedImageRectWidth,
                            views[eye].recommendedImageRectHeight, views[eye].recommendedImageRectWidth,
                            views[eye].recommendedImageRectHeight);
                    }
                }
                return result;
            }
            if (XR_SUCCEEDED(result) && m_cropActive &&
                viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
                return result;
            }
            if (XR_SUCCEEDED(result) && isVrSystem(systemId) && views) {
                // Determine the application resolution.
                // If a session is active, we use the values latched at session creation. Some applications like Unreal
                // or Unity seem to constantly poll xrEnumerateViewConfigurationViews() and we do not want to create a
                // tear.
                const auto upscaleMode =
                    m_vrSession != XR_NULL_HANDLE
                        ? m_upscaleMode
                        : m_configManager->peekEnumValue<config::ScalingType>(config::SettingScalingType);
                const auto settingScaling = m_vrSession != XR_NULL_HANDLE
                                                ? m_settingScaling
                                                : m_configManager->peekValue(config::SettingScaling);
                const auto settingAnamophic = m_vrSession != XR_NULL_HANDLE
                                                  ? m_settingAnamorphic
                                                  : m_configManager->peekValue(config::SettingAnamorphic);

                uint32_t inputWidth = m_displayWidth;
                uint32_t inputHeight = m_displayHeight;

                switch (upscaleMode) {
                case config::ScalingType::FSR:
                case config::ScalingType::NIS:
                case config::ScalingType::CAS: {
                    std::tie(inputWidth, inputHeight) = config::GetScaledDimensions(
                        settingScaling, settingAnamophic, m_displayWidth, m_displayHeight, 2);
                } break;

               
                case config::ScalingType::None:
                    break;

                default:
                    Log("Unknown upscaling type, falling back to no upscaling\n");
                    break;
                }

                // Override the recommended image size to account for scaling.
                for (uint32_t i = 0; i < *viewCountOutput; i++) {
                    views[i].recommendedImageRectWidth = m_cropActive && i < utilities::ViewCount
                                                             ? m_cropRecommendedWidth[i] : inputWidth;
                    views[i].recommendedImageRectHeight = m_cropActive && i < utilities::ViewCount
                                                              ? m_cropRecommendedHeight[i] : inputHeight;
                }
                if (!m_cropEnumerationLogged) {
                    for (uint32_t eye = 0; eye < std::min(*viewCountOutput, utilities::ViewCount); eye++) {
                        Log("[FOV-CROP] xrEnumerateViewConfigurationViews eye=%u raw=%ux%u delivered=%ux%u "
                            "active=%u mode=%s fov_percent=%d sharedMax=%ux%u\n",
                            eye, m_runtimeRecommendedWidth[eye], m_runtimeRecommendedHeight[eye],
                            views[eye].recommendedImageRectWidth, views[eye].recommendedImageRectHeight,
                            m_cropActive, m_cropActive ? (m_cropExact ? "exact" : "linear_fallback") : "inactive",
                            m_cropFovPercent, m_displayWidth, m_displayHeight);
                    }
                    m_cropEnumerationLogged = true;
                }

                static bool atLeastOnce = false;
                if (m_vrSession == XR_NULL_HANDLE || !atLeastOnce) {
                    if (inputWidth != m_displayWidth || inputHeight != m_displayHeight) {
                        Log("Upscaling from %ux%u to %ux%u (%u%%)\n",
                            inputWidth,
                            inputHeight,
                            m_displayWidth,
                            m_displayHeight,
                            (unsigned int)((((float)m_displayWidth / inputWidth) + 0.001f) * 100));
                    } else {
                        Log("Using OpenXR resolution (no upscaling): %ux%u\n", m_displayWidth, m_displayHeight);
                    }
                    atLeastOnce = true;
                }

                TraceLoggingWrite(
                    g_traceProvider,
                    "xrEnumerateViewConfigurationViews",
                    TLArg(fmt::format("{}x{}", inputWidth, inputHeight).c_str(), "AppResolution"),
                    TLArg(fmt::format("{}x{}", m_displayWidth, m_displayHeight).c_str(), "SystemResolution"));
            }

            return result;
        }

        XrResult xrCreateSession(XrInstance instance,
                                 const XrSessionCreateInfo* createInfo,
                                 XrSession* session) override {
            if (createInfo->type != XR_TYPE_SESSION_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSession",
                              TLPArg(instance, "Instance"),
                              TLArg((int)createInfo->systemId, "SystemId"),
                              TLArg(createInfo->createFlags, "CreateFlags"));

            // Force motion reprojection if requested.
            if (m_supportMotionReprojectionLock) {
                utilities::ToggleWindowsMixedRealityReprojection(
                    m_configManager->getEnumValue<config::MotionReprojection>(config::SettingMotionReprojection));
                m_isFrameThrottlingPossible = m_configManager->peekEnumValue<config::MotionReprojection>(
                                                  config::SettingMotionReprojection) != config::MotionReprojection::On;
            }

            const XrResult result = OpenXrApi::xrCreateSession(instance, createInfo, session);
            if (XR_SUCCEEDED(result) && isVrSystem(createInfo->systemId)) {
                if (isGraphicsNeutralApp() && m_gfxBisectMode >= 3) {
                    bool hasD3D12Binding = false;
                    auto binding = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
                    while (binding) {
                        hasD3D12Binding |= binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
                        binding = binding->next;
                    }
                    if (!hasD3D12Binding) {
                        Log("[GFX-BISECT] app=%s mode=%u requires D3D12; using mode=0\n",
                            m_applicationName.c_str(), m_gfxBisectMode);
                        m_gfxBisectMode = 0;
                    }
                }
                if (isGraphicsNeutralApp() && m_gfxBisectMode <= 2) {
                    const char* graphicsApi = "unknown";
                    const void* appQueue = nullptr;
                    const XrGraphicsBindingD3D12KHR* d3d12Bindings = nullptr;
                    auto entry = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
                    while (entry) {
                        if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                            graphicsApi = "D3D11";
                            break;
                        }
                        if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                            graphicsApi = "D3D12";
                            d3d12Bindings = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
                            appQueue = d3d12Bindings->queue;
                            break;
                        }
                        entry = entry->next;
                    }
                    if (m_gfxBisectMode > 0 && !d3d12Bindings) {
                        Log("[GFX-BISECT] app=%s mode=%u requires D3D12; using mode=0\n",
                            m_applicationName.c_str(), m_gfxBisectMode);
                        m_gfxBisectMode = 0;
                        m_gfxSubBisectMode = 0;
                        m_proxySyncEnabled = false;
                        m_proxyUsageEnabled = false;
                        m_proxyScopeEnabled = false;
                        m_proxyIndexEnabled = false;
                        Log("[PROXY-USAGE] unsupported reason=D3D12_binding_required\n");
                    }
                    if (m_gfxBisectMode > 0) {
                        // Keep the wrapper alive, but do not expose private images or initialize the processing chain.
                        m_bisectDevice = graphics::WrapD3D12Device(d3d12Bindings->device,
                                                                    d3d12Bindings->queue,
                                                                    m_configManager,
                                                                    false,
                                                                    true /* forceDisableInterceptor */);
                        if (m_proxySyncEnabled) {
                            m_bisectDevice->configureProxySyncDiagnostic(m_proxySyncMode, d3d12Bindings->queue);
                            m_proxyTimingLogs = 0;
                            m_proxyTimingFrame = 0;
                            const char* copyPoint = m_proxyTimingMode == 2 ? "endFrame" : "release";
                            const char* runtimeReleasePoint = m_proxyTimingMode == 0 ? "release" : "endFrame";
                            Log("[PROXY-TIMING] app=%s mode=%u base_submode=2 sync_mode=%u slots=32 "
                                "allocator_safe=1 wait_current_copy=1 copy_point=%s runtime_release_point=%s "
                                "color=proxy depth=direct overlays=0 postprocess=0 interceptor=0 analyzer=0\n",
                                m_applicationName.c_str(), m_proxyTimingMode, m_proxySyncMode,
                                copyPoint, runtimeReleasePoint);
                        }
                    }
                    m_metroGraphicsSession = *session;
                    if (m_proxyIndexEnabled) {
                        m_proxyIndexTrackers.clear();
                        m_proxyIndexStrict = true;
                        m_proxyIndexReason.clear();
                        m_proxyIndexAcquireCount = m_proxyIndexSuccessfulWaitCount = m_proxyIndexTimeoutCount = 0;
                        m_proxyIndexReleaseCount = m_proxyIndexLegacyMismatchCount = m_proxyIndexFifoErrors = 0;
                        m_proxyIndexPendingAtDestroy = 0;
                        m_proxyIndexAcquireLogs = m_proxyIndexWaitLogs = m_proxyIndexReleaseLogs = 0;
                        m_proxyIndexErrorLogs = m_proxyIndexTeardownLogs = 0;
                    }
                    if (m_proxyUsageEnabled && d3d12Bindings) {
                        beginProxyUsageSession(*session, d3d12Bindings->device);
                        if (m_proxyScopeEnabled) beginProxyScopeSession(d3d12Bindings->queue);
                    }
                    Log("[GFX-NEUTRAL] active app=%s session=%p api=%s app_queue=%p "
                        "toolkit_device_wrapper=%u interceptor=0 frame_analyzer=0 "
                        "toolkit_command_lists=%u toolkit_fences=%u\n",
                        m_applicationName.c_str(), *session, graphicsApi, appQueue,
                        m_gfxBisectMode > 0, m_gfxBisectMode > 0, m_gfxBisectMode > 0);
                    Log("[GFX-BISECT] app=%s mode=%u proxy=0 intermediate=0 delayed_release=0 "
                        "copy=0 interceptor=0 analyzer=0 command_lists=%u fences=%u flush=%s\n",
                        m_applicationName.c_str(), m_gfxBisectMode, m_gfxBisectMode > 0,
                        m_gfxBisectMode > 0, m_gfxBisectMode == 2 ? "per_frame" :
                                                    m_gfxBisectMode == 1 ? "setup_only" : "none");
                    logGraphicsSubBisectMode();
                    TraceLoggingWrite(g_traceProvider, "xrCreateSession", TLPArg(*session, "Session"));
                    return result;
                }
                // Get the graphics device.
                const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
                while (entry) {
                    if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                        TraceLoggingWrite(g_traceProvider, "UseD3D11");

                        const XrGraphicsBindingD3D11KHR* d3dBindings =
                            reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry);
                        // Workaround: On Oculus, we must delay the initialization of Detour.
                        const bool enableOculusQuirk = m_runtimeName.find("Oculus") != std::string::npos;
                        m_graphicsDevice =
                            graphics::WrapD3D11Device(d3dBindings->device, m_configManager, enableOculusQuirk);
                        break;
                    } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                        TraceLoggingWrite(g_traceProvider, "UseD3D12");

                        const XrGraphicsBindingD3D12KHR* d3dBindings =
                            reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
                        // Workaround: On Varjo, we must use intermediate textures with D3D11on12.
                        const bool enableVarjoQuirk = m_runtimeName.find("Varjo") != std::string::npos;
                        m_graphicsDevice = graphics::WrapD3D12Device(
                            d3dBindings->device, d3dBindings->queue, m_configManager, enableVarjoQuirk,
                            isGraphicsNeutralApp() && m_gfxBisectMode == 3);
                        break;
                    }

                    entry = entry->next;
                }

                if (m_graphicsDevice) {
                    // Initialize the other resources.

                    m_upscaleMode = m_configManager->getEnumValue<config::ScalingType>(config::SettingScalingType);
                    if (m_upscaleMode == config::ScalingType::NIS || 
                        m_upscaleMode == config::ScalingType::FSR ||
                        m_upscaleMode == config::ScalingType::CAS
                    ) {
                        m_settingScaling = m_configManager->peekValue(config::SettingScaling);
                        m_settingAnamorphic = m_configManager->peekValue(config::SettingAnamorphic);
                    }

                    switch (m_upscaleMode) {
                    case config::ScalingType::NIS:
                        m_upscaler = graphics::CreateNISUpscaler(
                            m_configManager, m_graphicsDevice, m_settingScaling, m_settingAnamorphic);
                        break;

                    case config::ScalingType::FSR:
                        m_upscaler = graphics::CreateFSRUpscaler(
                            m_configManager, m_graphicsDevice, m_settingScaling, m_settingAnamorphic);
                        break;

                    case config::ScalingType::CAS:
                        m_upscaler = graphics::CreateCASUpscaler(
                            m_configManager, m_graphicsDevice, m_settingScaling, m_settingAnamorphic);
                        break;

                    case config::ScalingType::None:
                        break;

                    default:
                        Log("Unknown upscaling type, falling back to no upscaling\n");
                        m_upscaleMode = config::ScalingType::None;
                        break;
                    }

                    uint32_t renderWidth = m_displayWidth;
                    uint32_t renderHeight = m_displayHeight;
                    if (m_upscaleMode == config::ScalingType::NIS || 
                        m_upscaleMode == config::ScalingType::FSR ||
                        m_upscaleMode == config::ScalingType::CAS
                    ) {
                        std::tie(renderWidth, renderHeight) = config::GetScaledDimensions(
                            m_settingScaling, m_settingAnamorphic, m_displayWidth, m_displayHeight, 2);

                        // Per FSR SDK documentation.
                        m_mipMapBiasForUpscaling = -std::log2f(static_cast<float>(m_displayWidth * m_displayHeight) /
                                                               (renderWidth * renderHeight));
                        Log("MipMap biasing for upscaling is: %.3f\n", m_mipMapBiasForUpscaling);
                    }

                    m_postProcessor = graphics::CreateImageProcessor(m_configManager, m_graphicsDevice);

                    if (m_graphicsDevice->isEventsSupported()) {
                        if (!m_configManager->getValue("disable_frame_analyzer")) {
                            graphics::FrameAnalyzerHeuristic heuristic = graphics::FrameAnalyzerHeuristic::Unknown;

                            // TODO: Override heuristic per-app if needed.

                            m_frameAnalyzer = graphics::CreateFrameAnalyzer(
                                m_configManager, m_graphicsDevice, m_displayWidth, m_displayHeight, heuristic);
                        }

                        m_variableRateShader =
                            graphics::CreateVariableRateShader(*this,
                                                               m_configManager,
                                                               m_graphicsDevice,
                                                               m_eyeTracker,
                                                               renderWidth,
                                                               renderHeight,
                                                               m_displayWidth,
                                                               m_displayHeight,
                                                               !m_isOpenComposite && m_hasVisibilityMaskKHR,
                                                               m_isUnity);

                        // Register intercepted events.
                        m_graphicsDevice->registerSetRenderTargetEvent(
                            [&](std::shared_ptr<graphics::IContext> context,
                                std::shared_ptr<graphics::ITexture> renderTarget) {
                                if (!m_isInFrame) {
                                    return;
                                }

                                if (m_frameAnalyzer) {
                                    m_frameAnalyzer->onSetRenderTarget(context, renderTarget);
                                    const auto& eyeHint = m_frameAnalyzer->getEyeHint();
                                    if (eyeHint.has_value()) {
                                        m_stats.hasColorBuffer[(int)eyeHint.value()] = true;
                                    }
                                }
                                if (m_variableRateShader) {
                                    if (m_variableRateShader->onSetRenderTarget(
                                            context,
                                            renderTarget,
                                            m_frameAnalyzer ? m_frameAnalyzer->getEyeHint() : std::nullopt)) {
                                        m_stats.numRenderTargetsWithVRS++;
                                    }
                                }
                            });
                        m_graphicsDevice->registerUnsetRenderTargetEvent(
                            [&](std::shared_ptr<graphics::IContext> context) {
                                if (!m_isInFrame) {
                                    return;
                                }

                                if (m_frameAnalyzer) {
                                    m_frameAnalyzer->onUnsetRenderTarget(context);
                                }
                                if (m_variableRateShader) {
                                    m_variableRateShader->onUnsetRenderTarget(context);
                                }
                            });
                        m_graphicsDevice->registerCopyTextureEvent([&](std::shared_ptr<graphics::IContext> context,
                                                                       std::shared_ptr<graphics::ITexture> source,
                                                                       std::shared_ptr<graphics::ITexture> destination,
                                                                       int sourceSlice,
                                                                       int destinationSlice) {
                            if (!m_isInFrame) {
                                return;
                            }

                            if (m_frameAnalyzer) {
                                m_frameAnalyzer->onCopyTexture(source, destination, sourceSlice, destinationSlice);
                            }
                        });
                    }

                    m_performanceCounters.appCpuTimer = utilities::CreateCpuTimer();
                    m_performanceCounters.renderCpuTimer = utilities::CreateCpuTimer();
                    m_performanceCounters.waitCpuTimer = utilities::CreateCpuTimer();
                    m_performanceCounters.endFrameCpuTimer = utilities::CreateCpuTimer();
                    m_performanceCounters.overlayCpuTimer = utilities::CreateCpuTimer();
                    m_performanceCounters.handTrackingTimer = utilities::CreateCpuTimer();

                    for (unsigned int i = 0; i <= GpuTimerLatency; i++) {
                        m_performanceCounters.appGpuTimer[i] = m_graphicsDevice->createTimer();
                        m_performanceCounters.overlayGpuTimer[i] = m_graphicsDevice->createTimer();
                    }

                    m_performanceCounters.lastWindowStart = std::chrono::steady_clock::now();

                    {
                        menu::MenuInfo menuInfo;
                        menuInfo.displayWidth = m_displayWidth;
                        menuInfo.displayHeight = m_displayHeight;
                        menuInfo.keyModifiers = m_keyModifiers;
                        menuInfo.isHandTrackingSupported = m_supportHandTracking;
                        menuInfo.isPredictionDampeningSupported = m_hasPerformanceCounterKHR;
                        menuInfo.maxDisplayHeight = m_maxDisplayHeight;
                        menuInfo.resolutionHeightRatio = m_resolutionHeightRatio;
                        menuInfo.isMotionReprojectionRateSupported = m_supportMotionReprojectionLock;
                        menuInfo.displayRefreshRate =
                            utilities::RegGetDword(
                                HKEY_LOCAL_MACHINE,
                                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Holographic\\DisplayThrottling",
                                L"ThrottleFramerate")
                                    .value_or(0)
                                ? 60u
                                : 90u;
                        menuInfo.variableRateShaderMaxRate =
                            m_variableRateShader ? m_variableRateShader->getMaxRate() : 0;
                        menuInfo.isEyeTrackingSupported = m_supportEyeTracking;
                        menuInfo.isEyeTrackingProjectionDistanceSupported =
                            m_eyeTracker ? m_eyeTracker->isProjectionDistanceSupported() : false;
                        menuInfo.isVisibilityMaskSupported = m_hasVisibilityMaskKHR;
                        // Our HAM override does not seem to work with OpenComposite.
                        menuInfo.isVisibilityMaskOverrideSupported = !m_isOpenComposite && m_hasVisibilityMaskKHR;
                        menuInfo.isCACorrectionNeed = m_configManager->isDeveloper() || m_systemName == "AERO" ||
                                                      m_configManager->getValue("allow_ca_correction");
                        menuInfo.cropActive = m_cropActive;
                        menuInfo.cropExact = m_cropExact;
                        menuInfo.runtimeName = m_runtimeName;

                        m_menuHandler = menu::CreateMenuHandler(m_configManager, m_graphicsDevice, menuInfo);
                    }

                    // Create a reference space to calculate projection views.
                    {
                        XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
                                                                            nullptr};
                        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                        referenceSpaceCreateInfo.poseInReferenceSpace = Pose::Identity();
                        CHECK_XRCMD(xrCreateReferenceSpace(*session, &referenceSpaceCreateInfo, &m_viewSpace));
                    }

                    if (m_handTracker) {
                        m_handTracker->beginSession(*session, m_graphicsDevice);
                    }
                    if (m_eyeTracker) {
                        m_isSuggestingToolkitEyeBinding = true;
                        try {
                            m_eyeTracker->beginSession(*session);
                        } catch (...) {
                            m_isSuggestingToolkitEyeBinding = false;
                            throw;
                        }
                        m_isSuggestingToolkitEyeBinding = false;
                        m_eyeTracker->setActionSetReady(!m_isOpenComposite);
                    }
                    DiagnosticLog("session created session=%p eye_set=%p opencomposite=%u", *session,
                                  m_eyeTracker ? m_eyeTracker->getActionSet() : XR_NULL_HANDLE, m_isOpenComposite);

                    // Make sure we perform calibration again. We pass these values to the menu and FFR, so in the case
                    // of multi-session applications, we must push those values again.
                    m_needCalibrateEyeProjections = true;

                    // Attachment and sync belong to this session, even when the tracker survives it.
                    m_isActionSetUsed = false;
                    m_isActionSetAttached = false;
                    m_isEyeActionSetSynced = false;
                    m_lastSyncError = XR_SUCCESS;
                    m_artificialActionsLogged = false;
                    m_cropFovLogged = false;
                    m_cropCalibrationChecked = false;
                    m_cropInvalidFovLogged = false;

                    // Remember the XrSession to use.
                    m_vrSession = *session;
                    if (isGraphicsNeutralApp() && m_gfxBisectMode >= 3) {
                        Log("[GFX-BISECT] app=%s mode=%u proxy=1 intermediate=conditional "
                            "delayed_release=1 copy=processing_chain interceptor=%u analyzer=%u "
                            "command_lists=1 fences=1 flush=normal\n",
                            m_applicationName.c_str(), m_gfxBisectMode,
                            m_graphicsDevice->isEventsSupported(), !!m_frameAnalyzer);
                        logGraphicsSubBisectMode();
                    }
                } else {
                    Log("Unsupported graphics runtime.\n");
                }

                TraceLoggingWrite(g_traceProvider, "xrCreateSession", TLPArg(*session, "Session"));
            }

            return result;
        }

        XrResult xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo) override {
            if (beginInfo->type != XR_TYPE_SESSION_BEGIN_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(
                g_traceProvider,
                "xrBeginSession",
                TLPArg(session, "Session"),
                TLArg(xr::ToCString(beginInfo->primaryViewConfigurationType), "PrimaryViewConfigurationType"));

            const XrResult result = OpenXrApi::xrBeginSession(session, beginInfo);
            DiagnosticLog("xrBeginSession session=%p result=%s", session, xr::ToCString(result));
            if (XR_SUCCEEDED(result) && isVrSession(session)) {
                m_configManager->setActiveSession(m_applicationName);

                // Bump up timer precision for this process.
                utilities::EnableHighPrecisionTimer();

                if (m_variableRateShader) {
                    m_variableRateShader->beginSession(session);
                }
            }

            return result;
        }

        XrResult xrEndSession(XrSession session) override {
            TraceLoggingWrite(g_traceProvider, "xrEndSession", TLPArg(session, "Session"));

            const XrResult result = OpenXrApi::xrEndSession(session);
            DiagnosticLog("xrEndSession session=%p result=%s", session, xr::ToCString(result));
            if (XR_SUCCEEDED(result) && isVrSession(session)) {
                if (m_variableRateShader) {
                    m_variableRateShader->endSession();
                }

                // Action sets remain attached to the session, but gaze must be synced again after restart.
                m_isEyeActionSetSynced = false;
                if (m_eyeTracker && m_isOpenComposite) {
                    m_eyeTracker->setActionSetReady(false);
                }

                utilities::RestoreTimerPrecision();

                m_configManager->setActiveSession("");
            }

            return result;
        }

        XrResult xrDestroySession(XrSession session) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySession", TLPArg(session, "Session"));
            DiagnosticLog("xrDestroySession session=%p", session);

            // Prepare for shutdown
            if (isVrSession(session)) {
                if (m_configManager) {
                    m_configManager->setActiveSession("");
                }

                // Wait for any pending operation to complete.
                if (m_graphicsDevice) {
                    if (m_asyncWaitPromise.valid()) {
                        m_asyncWaitPromise.wait_for(5s);
                        m_asyncWaitPromise = {};
                    }

                    m_graphicsDevice->blockCallbacks();
                    m_graphicsDevice->flushContext(true);
                }

                // Cleanup session resources.
                if (m_viewSpace != XR_NULL_HANDLE) {
                    xrDestroySpace(m_viewSpace);
                    m_viewSpace = XR_NULL_HANDLE;
                }
                if (m_handTracker) {
                    m_handTracker->endSession();
                }
                if (m_eyeTracker) {
                    m_eyeTracker->endSession();
                }
                m_toolkitEyeGazeBinding.reset();
                // Suggested application bindings belong to the instance, not the session. A new session can
                // recreate our gaze action without the application suggesting its bindings again.
                if (m_menuSwapchain != XR_NULL_HANDLE) {
                    xrDestroySwapchain(m_menuSwapchain);
                    m_menuSwapchain = XR_NULL_HANDLE;
                }
            }

            const XrResult result = OpenXrApi::xrDestroySession(session);

            if (XR_SUCCEEDED(result) && isMetroGraphicsSession(session)) {
                if (m_proxyUsageEnabled) {
                    finishProxyUsageSession();
                }
                if (m_proxyIndexEnabled) finishProxyIndexSession();
                m_swapchains.clear();
                if (m_bisectDevice) {
                    // Keep the D3D12 wrapper alive until its diagnostic textures have been released.
                    m_bisectDevice->flushContext(true);
                    m_bisectDevice->shutdown();
                    m_bisectDevice.reset();
                }
                Log("[GFX-NEUTRAL] session destroyed session=%p direct_swapchains=%zu\n",
                    session, m_metroSwapchainIndices.size());
                m_metroSwapchainIndices.clear();
                m_subPendingDirectRelease.clear();
                m_subProxySwapchains.clear();
                m_proxyIndexTrackers.clear();
                m_subProxyCopyCompleted.clear();
                m_proxyTimingPendingOrder.clear();
                m_proxyTimingCopies.clear();
                m_metroGraphicsSession = XR_NULL_HANDLE;
            }

            if (XR_SUCCEEDED(result) && isVrSession(session)) {
                // Cleanup our resources.
                m_upscaler.reset();
                m_postProcessor.reset();
                m_frameAnalyzer.reset();
                m_variableRateShader.reset();
                for (unsigned int i = 0; i <= GpuTimerLatency; i++) {
                    m_performanceCounters.appGpuTimer[i].reset();
                    m_performanceCounters.overlayGpuTimer[i].reset();
                }
                m_performanceCounters.appCpuTimer.reset();
                m_performanceCounters.renderCpuTimer.reset();
                m_performanceCounters.waitCpuTimer.reset();
                m_performanceCounters.endFrameCpuTimer.reset();
                m_performanceCounters.overlayCpuTimer.reset();
                m_swapchains.clear();
                m_menuSwapchainImages.clear();
                m_menuHandler.reset();
                if (m_graphicsDevice) {
                    m_graphicsDevice->shutdown();
                }
                m_graphicsDevice.reset();

                // We intentionally do not reset hand/eye trackers since they are tied to the instance, not session.

                m_vrSession = XR_NULL_HANDLE;
                m_isActionSetUsed = false;
                m_isActionSetAttached = false;
                m_isEyeActionSetSynced = false;

                // A good check to ensure there are no resources leak is to confirm that the graphics device is
                // destroyed _before_ we see this message.
                // eg:
                // 2022-01-01 17:15:35 -0800: D3D11Device destroyed
                // 2022-01-01 17:15:35 -0800: Session destroyed
                // If the order is reversed or the Device is destructed missing, then it means that we are not cleaning
                // up the resources properly.
                Log("Session destroyed\n");

                utilities::RestoreTimerPrecision();
            }

            return result;
        }

        XrResult xrCreateSwapchain(XrSession session,
                                   const XrSwapchainCreateInfo* createInfo,
                                   XrSwapchain* swapchain) override {
            if (createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSwapchain",
                              TLPArg(session, "Session"),
                              TLArg(createInfo->arraySize, "ArraySize"),
                              TLArg(createInfo->width, "Width"),
                              TLArg(createInfo->height, "Height"),
                              TLArg(createInfo->createFlags, "CreateFlags"),
                              TLArg(createInfo->format, "Format"),
                              TLArg(createInfo->faceCount, "FaceCount"),
                              TLArg(createInfo->mipCount, "MipCount"),
                              TLArg(createInfo->sampleCount, "SampleCount"),
                              TLArg(createInfo->usageFlags, "UsageFlags"));

            if (isMetroGraphicsSession(session)) {
                if (m_gfxSubBisectMode == 2 || m_gfxSubBisectMode == 3) {
                    // The runtime owns the swapchain. Only its color images are replaced for the app.
                    const bool isDepth = (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
                    const auto scope = m_proxyScopeEnabled ? selectProxyScope(*createInfo) : proxy_scope::Decision{};
                    const bool hasProxy = !isDepth && (!m_proxyScopeEnabled || scope.hasProxy);
                    const auto downstreamCreateInfo = proxy_usage::DownstreamCreateInfo(
                        *createInfo, m_proxyScopeEnabled ? 0 : (m_proxyUsageEnabled ? m_proxyUsageMode : 0), hasProxy);
                    const XrResult result = OpenXrApi::xrCreateSwapchain(session, &downstreamCreateInfo, swapchain);
                    if (XR_FAILED(result)) {
                        if (m_proxyScopeEnabled) invalidateProxyScope("runtime_create_rejected");
                        if (m_proxyUsageEnabled) {
                            m_proxyUsageRunStrict = false;
                            Log("[PROXY-USAGE] mode=%u event=create_rejected session=%p original_usage=0x%llx "
                                "downstream_usage=0x%llx result=%s retry=0 strict_comparison=0 "
                                "reason=runtime_rejected_variant\n", m_proxyUsageMode, session,
                                static_cast<unsigned long long>(createInfo->usageFlags),
                                static_cast<unsigned long long>(downstreamCreateInfo.usageFlags), xr::ToCString(result));
                        }
                        return result;
                    }
                    uint32_t imageCount = 0;
                    CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(*swapchain, 0, &imageCount, nullptr));
                    std::vector<XrSwapchainImageD3D12KHR> runtimeImages(imageCount,
                                                                         {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                    CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(
                        *swapchain, imageCount, &imageCount,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(runtimeImages.data())));
                    SwapchainState state;
                    state.requestedWidth = createInfo->width;
                    state.requestedHeight = createInfo->height;
                    state.requestedArraySize = createInfo->arraySize;
                    const D3D12_RESOURCE_STATES initialState = isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE :
                        (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) ?
                            D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COMMON;
                    for (uint32_t i = 0; i < imageCount; ++i) {
                        SwapchainImages images;
                        images.runtimeTexture = graphics::WrapD3D12Texture(
                            m_bisectDevice, *createInfo, runtimeImages[i].texture, initialState,
                            fmt::format("Sub-bisect runtime swapchain {} TEX2D", i));
                        if (!hasProxy) {
                            images.appTexture = images.runtimeTexture;
                        } else {
                            const auto format = static_cast<int64_t>(runtimeImages[i].texture->GetDesc().Format);
                            images.appTexture = m_bisectDevice->createTexture(
                                *createInfo, fmt::format("Sub-bisect app swapchain {} TEX2D", i), format);
                        }
                        state.images.push_back(std::move(images));
                    }
                    m_swapchains.insert_or_assign(*swapchain, std::move(state));
                    m_subProxySwapchains.insert_or_assign(*swapchain, hasProxy);
                    if (m_proxyIndexEnabled && hasProxy && scope.strict && scope.referenceRole == 1)
                        m_proxyIndexTrackers.emplace(*swapchain, proxy_index::Tracker{});
                    m_subProxyCopyCompleted.insert_or_assign(*swapchain, false);
                    if (m_proxyUsageEnabled) {
                        if (m_proxyScopeEnabled) {
                            m_proxyScopeDecisions.emplace(m_proxyUsageCreationId + 1, scope);
                        }
                        recordProxyUsageCreation(session, *swapchain, *createInfo, downstreamCreateInfo, initialState);
                    }
                    Log("[GFX-SUBBISECT] create app=%s submode=%u swapchain=%p requested=%ux%u "
                        "arraySize=%u format=%lld sampleCount=%u usageFlags=0x%llx images=%u proxy=%u\n",
                        m_applicationName.c_str(), m_gfxSubBisectMode, *swapchain, createInfo->width,
                        createInfo->height, createInfo->arraySize, static_cast<long long>(createInfo->format),
                        createInfo->sampleCount, static_cast<unsigned long long>(createInfo->usageFlags),
                        imageCount, hasProxy);
                    return result;
                }
                // Every image returned to this diagnostic application is owned by the runtime.
                const XrResult result = OpenXrApi::xrCreateSwapchain(session, createInfo, swapchain);
                if (XR_SUCCEEDED(result)) {
                    m_metroSwapchainIndices.insert_or_assign(*swapchain, UINT32_MAX);
                    if (m_gfxSubBisectMode == 1) {
                        m_subPendingDirectRelease.insert_or_assign(*swapchain, false);
                    }
                }
                Log("[GFX-NEUTRAL] create swapchain=%p result=%s requested=%ux%u arraySize=%u "
                    "format=%lld sampleCount=%u usageFlags=0x%llx runtime=%ux%u "
                    "proxy=0 intermediate=0 additional_usage=0\n",
                    XR_SUCCEEDED(result) ? *swapchain : XR_NULL_HANDLE, xr::ToCString(result),
                    createInfo->width, createInfo->height, createInfo->arraySize,
                    static_cast<long long>(createInfo->format), createInfo->sampleCount,
                    static_cast<unsigned long long>(createInfo->usageFlags),
                    createInfo->width, createInfo->height);
                return result;
            }

            if (!isVrSession(session) || !m_graphicsDevice) {
                return OpenXrApi::xrCreateSwapchain(session, createInfo, swapchain);
            }

            // We do no do any processing to depth buffer, but we like to have them for other things like occlusion when
            // drawing.
            const bool isDepth = createInfo->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

            Log("Creating swapchain with dimensions=%ux%u, arraySize=%u, mipCount=%u, sampleCount=%u, format=%d, "
                "usage=0x%x\n",
                createInfo->width,
                createInfo->height,
                createInfo->arraySize,
                createInfo->mipCount,
                createInfo->sampleCount,
                createInfo->format,
                createInfo->usageFlags);
            Log("[FOV-CROP] xrCreateSwapchain session=%p requested=%ux%u arraySize=%u format=%lld "
                "sampleCount=%u usageFlags=0x%llx depth=%u crop_active=%u\n",
                session, createInfo->width, createInfo->height, createInfo->arraySize,
                static_cast<long long>(createInfo->format), createInfo->sampleCount,
                static_cast<unsigned long long>(createInfo->usageFlags), isDepth, m_cropActive);

            XrSwapchainCreateInfo chainCreateInfo = *createInfo;
            if (!isDepth) {
                // Modify the swapchain to handle our processing chain (eg: change resolution and/or usage.

                if (m_upscaleMode == config::ScalingType::NIS || 
                    m_upscaleMode == config::ScalingType::FSR ||
                    m_upscaleMode == config::ScalingType::CAS
                ) {
                    float horizontalScaleFactor;
                    float verticalScaleFactor;
                    std::tie(horizontalScaleFactor, verticalScaleFactor) =
                        config::GetScalingFactors(m_settingScaling, m_settingAnamorphic);

                    chainCreateInfo.width = roundUp((uint32_t)std::ceil(createInfo->width * horizontalScaleFactor), 2);
                    chainCreateInfo.height = roundUp((uint32_t)std::ceil(createInfo->height * verticalScaleFactor), 2);
                }

                // The post processor will draw a full-screen quad onto the final swapchain.
                chainCreateInfo.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            }

            const XrResult result = OpenXrApi::xrCreateSwapchain(session, &chainCreateInfo, swapchain);
            if (XR_SUCCEEDED(result)) {
                uint32_t imageCount;
                CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(*swapchain, 0, &imageCount, nullptr));

                SwapchainState swapchainState;
                swapchainState.requestedWidth = createInfo->width;
                swapchainState.requestedHeight = createInfo->height;
                swapchainState.requestedArraySize = createInfo->arraySize;
                int64_t overrideFormat = 0;
                if (m_graphicsDevice->getApi() == graphics::Api::D3D11) {
                    std::vector<XrSwapchainImageD3D11KHR> d3dImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                    CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(
                        *swapchain,
                        imageCount,
                        &imageCount,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(d3dImages.data())));

                    // Dump the descriptor for the first texture returned by the runtime for debug purposes.
                    {
                        D3D11_TEXTURE2D_DESC desc;
                        d3dImages[0].texture->GetDesc(&desc);
                        TraceLoggingWrite(g_traceProvider,
                                          "RuntimeSwapchain",
                                          TLArg(desc.Width, "Width"),
                                          TLArg(desc.Height, "Height"),
                                          TLArg(desc.ArraySize, "ArraySize"),
                                          TLArg(desc.MipLevels, "MipCount"),
                                          TLArg(desc.SampleDesc.Count, "SampleCount"),
                                          TLArg((int)desc.Format, "Format"),
                                          TLArg((int)desc.Usage, "Usage"),
                                          TLArg(desc.BindFlags, "BindFlags"),
                                          TLArg(desc.CPUAccessFlags, "CPUAccessFlags"),
                                          TLArg(desc.MiscFlags, "MiscFlags"));

                        // Make sure to create the app texture typeless.
                        overrideFormat = (int64_t)desc.Format;
                    }

                    for (uint32_t i = 0; i < imageCount; i++) {
                        SwapchainImages images;

                        // Store the runtime images into the state (last entry in the processing chain).
                        images.runtimeTexture =
                            graphics::WrapD3D11Texture(m_graphicsDevice,
                                                       chainCreateInfo,
                                                       d3dImages[i].texture,
                                                       fmt::format("Runtime swapchain {} TEX2D", i));

                        swapchainState.images.push_back(std::move(images));
                    }
                } else if (m_graphicsDevice->getApi() == graphics::Api::D3D12) {
                    std::vector<XrSwapchainImageD3D12KHR> d3dImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                    CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(
                        *swapchain,
                        imageCount,
                        &imageCount,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(d3dImages.data())));

                    // Dump the descriptor for the first texture returned by the runtime for debug purposes.
                    {
                        const auto& desc = d3dImages[0].texture->GetDesc();
                        TraceLoggingWrite(g_traceProvider,
                                          "RuntimeSwapchain",
                                          TLArg(desc.Width, "Width"),
                                          TLArg(desc.Height, "Height"),
                                          TLArg(desc.DepthOrArraySize, "ArraySize"),
                                          TLArg(desc.MipLevels, "MipCount"),
                                          TLArg(desc.SampleDesc.Count, "SampleCount"),
                                          TLArg((int)desc.Format, "Format"),
                                          TLArg((int)desc.Flags, "Flags"));

                        // Make sure to create the app texture typeless.
                        overrideFormat = (int64_t)desc.Format;
                    }

                    for (uint32_t i = 0; i < imageCount; i++) {
                        SwapchainImages images;

                        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
                        if ((chainCreateInfo.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) {
                            initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        } else if ((chainCreateInfo.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
                            initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                        }

                        // Store the runtime images into the state (last entry in the processing chain).
                        images.runtimeTexture =
                            graphics::WrapD3D12Texture(m_graphicsDevice,
                                                       chainCreateInfo,
                                                       d3dImages[i].texture,
                                                       initialState,
                                                       fmt::format("Runtime swapchain {} TEX2D", i));

                        swapchainState.images.push_back(std::move(images));
                    }
                } else {
                    throw std::runtime_error("Unsupported graphics runtime");
                }

                for (uint32_t i = 0; i < imageCount; i++) {
                    SwapchainImages& images = swapchainState.images[i];

                    if (!isDepth) {
                        // Create an app texture with the exact specification requested (lower resolution in case of
                        // upscaling).
                        XrSwapchainCreateInfo inputCreateInfo = *createInfo;

                        // Both post-processor and upscalers need to do sampling.
                        inputCreateInfo.usageFlags |= XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

                        images.appTexture = m_graphicsDevice->createTexture(
                            inputCreateInfo, fmt::format("App swapchain {} TEX2D", i), overrideFormat);

                        for (uint32_t i = 0; i < utilities::ViewCount; i++) {
                            images.upscalingTimers[i] = m_graphicsDevice->createTimer();
                            images.postProcessingTimers[i] = m_graphicsDevice->createTimer();
                        }
                    } else {
                        images.appTexture = images.runtimeTexture;
                    }
                }

                m_swapchains.insert_or_assign(*swapchain, swapchainState);

                const auto appInfo = swapchainState.images[0].appTexture->getInfo();
                const auto runtimeInfo = swapchainState.images[0].runtimeTexture->getInfo();
                Log("[FOV-CROP] xrCreateSwapchain result=%s swapchain=%p forwarded=%ux%u "
                    "appTexture=%ux%u arraySize=%u runtimeTexture=%ux%u arraySize=%u\n",
                    xr::ToCString(result), *swapchain, chainCreateInfo.width, chainCreateInfo.height,
                    appInfo.width, appInfo.height, appInfo.arraySize,
                    runtimeInfo.width, runtimeInfo.height, runtimeInfo.arraySize);

                TraceLoggingWrite(g_traceProvider, "xrCreateSwapchain", TLPArg(*swapchain, "Swapchain"));
            }

            return result;
        }

        XrResult xrDestroySwapchain(XrSwapchain swapchain) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySwapchain", TLPArg(swapchain, "Swapchain"));

            // In Turbo Mode, make sure there is no pending frame that may potentially hold onto the swapchain.
            {
                std::unique_lock lock(m_frameLock);

                if (m_asyncWaitPromise.valid()) {
                    TraceLocalActivity(local);

                    TraceLoggingWriteStart(local, "AsyncWaitNow");
                    m_asyncWaitPromise.wait();
                    TraceLoggingWriteStop(local, "AsyncWaitNow");
                }
            }

            const XrResult result = OpenXrApi::xrDestroySwapchain(swapchain);
            if (XR_SUCCEEDED(result)) {
                if (m_proxyUsageEnabled && m_proxyUsageSwapchains.count(swapchain)) {
                    summarizeProxyUsage(swapchain);
                    m_proxyUsageSwapchains.erase(swapchain);
                }
                m_swapchains.erase(swapchain);
                m_subProxySwapchains.erase(swapchain);
                auto indexTracker = m_proxyIndexTrackers.find(swapchain);
                if (indexTracker != m_proxyIndexTrackers.end()) {
                    const size_t pending = indexTracker->second.queue.size();
                    m_proxyIndexPendingAtDestroy += pending;
                    if (pending && m_proxyIndexTeardownLogs++ < 8) {
                        Log("[PROXY-INDEX] event=destroy swapchain=%p pending=%zu queue=%s "
                            "teardown_only=1\n", swapchain, pending, indexTracker->second.state().c_str());
                    }
                    m_proxyIndexTrackers.erase(indexTracker);
                }
                m_subProxyCopyCompleted.erase(swapchain);
                m_proxyTimingCopies.erase(swapchain);
                m_proxyTimingPendingOrder.erase(std::remove(m_proxyTimingPendingOrder.begin(),
                    m_proxyTimingPendingOrder.end(), swapchain), m_proxyTimingPendingOrder.end());
                m_subPendingDirectRelease.erase(swapchain);
                if (m_metroSwapchainIndices.erase(swapchain)) {
                    Log("[GFX-NEUTRAL] destroy swapchain=%p result=%s\n", swapchain, xr::ToCString(result));
                }
            }

            return result;
        }

        XrResult xrSuggestInteractionProfileBindings(
            XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings) override {
            if (suggestedBindings->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            const std::string interactionProfile = getPath(suggestedBindings->interactionProfile);
            const bool isEyeGazeProfile = interactionProfile == "/interaction_profiles/ext/eye_gaze_interaction";
            const bool isToolkitEyeBinding = isEyeGazeProfile && m_isSuggestingToolkitEyeBinding;
            const bool actionSetsAttached = m_isActionSetUsed || m_isActionSetAttached;

            TraceLoggingWrite(g_traceProvider,
                              "xrSuggestInteractionProfileBindings",
                              TLPArg(instance, "Instance"),
                              TLArg(interactionProfile.c_str(), "InteractionProfile"));

            for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; i++) {
                TraceLoggingWrite(g_traceProvider,
                                  "xrSuggestInteractionProfileBindings",
                                  TLPArg(suggestedBindings->suggestedBindings[i].action, "Action"),
                                  TLArg(getPath(suggestedBindings->suggestedBindings[i].binding).c_str(), "Path"));
            }

            XrInteractionProfileSuggestedBinding chainSuggestedBindings = *suggestedBindings;
            std::vector<XrActionSuggestedBinding> mergedBindings;
            std::optional<XrActionSuggestedBinding> toolkitBinding = m_toolkitEyeGazeBinding;
            uint32_t appBindingCount = 0;
            if (isEyeGazeProfile) {
                if (isToolkitEyeBinding && suggestedBindings->countSuggestedBindings == 1) {
                    toolkitBinding = suggestedBindings->suggestedBindings[0];
                }

                // Repeated suggestions replace the entire profile. Preserve the latest application list on both
                // sides of the eye tracker's internal suggestion, before any action set is attached.
                if (!actionSetsAttached && toolkitBinding) {
                    if (isToolkitEyeBinding) {
                        mergedBindings = m_appEyeGazeBindings;
                    } else if (suggestedBindings->countSuggestedBindings) {
                        mergedBindings.assign(suggestedBindings->suggestedBindings,
                                              suggestedBindings->suggestedBindings + suggestedBindings->countSuggestedBindings);
                    }
                    appBindingCount = static_cast<uint32_t>(mergedBindings.size());
                    const bool alreadyIncluded = std::any_of(
                        mergedBindings.begin(), mergedBindings.end(), [&](const XrActionSuggestedBinding& binding) {
                            return binding.action == toolkitBinding->action && binding.binding == toolkitBinding->binding;
                        });
                    if (!alreadyIncluded) {
                        mergedBindings.push_back(*toolkitBinding);
                    }
                    chainSuggestedBindings.countSuggestedBindings = static_cast<uint32_t>(mergedBindings.size());
                    chainSuggestedBindings.suggestedBindings = mergedBindings.data();
                }
            }

            if (m_configManager->getValue(config::SettingEyeDebugWithController)) {
                // We must drop calls to allow the controller override for debugging.
                if (suggestedBindings->countSuggestedBindings > 1 ||
                    interactionProfile != "/interaction_profiles/hp/mixed_reality_controller") {
                    return XR_SUCCESS;
                }
            }

            const XrResult result = OpenXrApi::xrSuggestInteractionProfileBindings(instance, &chainSuggestedBindings);
            if (isEyeGazeProfile) {
                if (XR_SUCCEEDED(result) && !actionSetsAttached) {
                    if (isToolkitEyeBinding) {
                        m_toolkitEyeGazeBinding = toolkitBinding;
                    } else {
                        if (suggestedBindings->countSuggestedBindings) {
                            m_appEyeGazeBindings.assign(suggestedBindings->suggestedBindings,
                                                        suggestedBindings->suggestedBindings + suggestedBindings->countSuggestedBindings);
                        } else {
                            m_appEyeGazeBindings.clear();
                        }
                    }
                }
                if (appBindingCount && toolkitBinding && !actionSetsAttached) {
                    DiagnosticLog("eye binding merge app_binding_count=%u toolkit_binding_present=1 merged_count=%u result=%s",
                                  appBindingCount, chainSuggestedBindings.countSuggestedBindings, xr::ToCString(result));
                } else if (XR_FAILED(result)) {
                    DiagnosticLog("eye binding suggestion failed result=%s attached=%u", xr::ToCString(result),
                                  actionSetsAttached);
                }
            }
            if (XR_SUCCEEDED(result) && m_handTracker) {
                m_handTracker->registerBindings(*suggestedBindings);
            }

            return result;
        }

        XrResult xrAttachSessionActionSets(XrSession session,
                                           const XrSessionActionSetsAttachInfo* attachInfo) override {
            if (attachInfo->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider, "xrAttachSessionActionSets", TLPArg(session, "Session"));
            for (uint32_t i = 0; i < attachInfo->countActionSets; i++) {
                TraceLoggingWrite(
                    g_traceProvider, "xrAttachSessionActionSets", TLPArg(attachInfo->actionSets[i], "ActionSet"));
            }

            XrSessionActionSetsAttachInfo chainAttachInfo = *attachInfo;
            std::vector<XrActionSet> newActionSets;
            if (m_eyeTracker && isVrSession(session)) {
                const auto eyeTrackerActionSet = m_eyeTracker->getActionSet();
                if (eyeTrackerActionSet != XR_NULL_HANDLE) {
                    newActionSets.resize(chainAttachInfo.countActionSets + 1);
                    memcpy(newActionSets.data(),
                           chainAttachInfo.actionSets,
                           chainAttachInfo.countActionSets * sizeof(XrActionSet));
                    uint32_t nextActionSetSlot = chainAttachInfo.countActionSets;

                    newActionSets[nextActionSetSlot++] = eyeTrackerActionSet;

                    chainAttachInfo.actionSets = newActionSets.data();
                    chainAttachInfo.countActionSets = nextActionSetSlot;
                }
            }

            const XrResult result = OpenXrApi::xrAttachSessionActionSets(session, &chainAttachInfo);
            if (XR_SUCCEEDED(result) && isVrSession(session)) {
                m_isActionSetUsed = attachInfo->countActionSets > 0;
                if (chainAttachInfo.countActionSets != attachInfo->countActionSets) {
                    m_isActionSetAttached = true;
                }
            }
            DiagnosticLog("xrAttachSessionActionSets session=%p result=%s app_count=%u forwarded_count=%u eye_included=%u eye_set_attached=%u",
                          session, xr::ToCString(result), attachInfo->countActionSets, chainAttachInfo.countActionSets,
                          chainAttachInfo.countActionSets != attachInfo->countActionSets,
                          m_isActionSetAttached);
            return result;
        }

        XrResult xrCreateAction(XrActionSet actionSet,
                                const XrActionCreateInfo* createInfo,
                                XrAction* action) override {
            if (createInfo->type != XR_TYPE_ACTION_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateAction",
                              TLPArg(actionSet, "ActionSet"),
                              TLArg(createInfo->actionName, "Name"),
                              TLArg(createInfo->localizedActionName, "LocalizedName"),
                              TLArg(xr::ToCString(createInfo->actionType), "Type"));
            for (uint32_t i = 0; i < createInfo->countSubactionPaths; i++) {
                TraceLoggingWrite(g_traceProvider,
                                  "xrCreateAction",
                                  TLArg(getPath(createInfo->subactionPaths[i]).c_str(), "SubactionPath"));
            }

            const XrResult result = OpenXrApi::xrCreateAction(actionSet, createInfo, action);
            if (XR_SUCCEEDED(result)) {
                m_actionSetByAction[*action] = actionSet;
                if (m_handTracker) {
                    m_handTracker->registerAction(*action, actionSet);
                }
                TraceLoggingWrite(g_traceProvider, "xrCreateAction", TLPArg(*action, "Action"));
            }

            return result;
        }

        XrResult xrDestroyAction(XrAction action) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroyAction", TLPArg(action, "Action"));

            const XrResult result = OpenXrApi::xrDestroyAction(action);
            if (XR_SUCCEEDED(result)) {
                m_actionSetByAction.erase(action);
                m_appEyeGazeBindings.erase(
                    std::remove_if(m_appEyeGazeBindings.begin(), m_appEyeGazeBindings.end(),
                                   [action](const XrActionSuggestedBinding& binding) { return binding.action == action; }),
                    m_appEyeGazeBindings.end());
                if (m_toolkitEyeGazeBinding && m_toolkitEyeGazeBinding->action == action) {
                    m_toolkitEyeGazeBinding.reset();
                }
                if (m_handTracker) {
                    m_handTracker->unregisterAction(action);
                }
            }

            return result;
        }

        XrResult xrDestroyActionSet(XrActionSet actionSet) override {
            const XrResult result = OpenXrApi::xrDestroyActionSet(actionSet);
            if (XR_SUCCEEDED(result)) {
                const auto belongsToSet = [&](XrAction action) {
                    const auto it = m_actionSetByAction.find(action);
                    return it != m_actionSetByAction.end() && it->second == actionSet;
                };
                m_appEyeGazeBindings.erase(
                    std::remove_if(m_appEyeGazeBindings.begin(), m_appEyeGazeBindings.end(),
                                   [&](const XrActionSuggestedBinding& binding) { return belongsToSet(binding.action); }),
                    m_appEyeGazeBindings.end());
                if (m_toolkitEyeGazeBinding && belongsToSet(m_toolkitEyeGazeBinding->action)) {
                    m_toolkitEyeGazeBinding.reset();
                }
                for (auto it = m_actionSetByAction.begin(); it != m_actionSetByAction.end();) {
                    if (it->second == actionSet) {
                        it = m_actionSetByAction.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            return result;
        }

        XrResult xrCreateActionSpace(XrSession session,
                                     const XrActionSpaceCreateInfo* createInfo,
                                     XrSpace* space) override {
            if (createInfo->type != XR_TYPE_ACTION_SPACE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateActionSpace",
                              TLPArg(session, "Session"),
                              TLPArg(createInfo->action, "Action"),
                              TLArg(getPath(createInfo->subactionPath).c_str(), "SubactionPath"),
                              TLArg(xr::ToString(createInfo->poseInActionSpace).c_str(), "PoseInActionSpace"));

            const XrResult result = OpenXrApi::xrCreateActionSpace(session, createInfo, space);
            if (XR_SUCCEEDED(result) && isVrSession(session)) {
                if (m_handTracker) {
                    // Keep track of the XrSpace for controllers, so we can override the behavior for them.
                    const std::string fullPath =
                        m_handTracker->getFullPath(createInfo->action, createInfo->subactionPath);
                    if (fullPath == "/user/hand/right/input/grip/pose" ||
                        fullPath == "/user/hand/right/input/aim/pose" ||
                        fullPath == "/user/hand/left/input/grip/pose" || fullPath == "/user/hand/left/input/aim/pose") {
                        m_handTracker->registerActionSpace(*space, fullPath, createInfo->poseInActionSpace);
                    }
                }

                TraceLoggingWrite(g_traceProvider, "xrCreateActionSpace", TLPArg(*space, "Space"));
            }

            return result;
        }

        XrResult xrDestroySpace(XrSpace space) override {
            const XrResult result = OpenXrApi::xrDestroySpace(space);
            if (XR_SUCCEEDED(result) && m_handTracker) {
                m_handTracker->unregisterActionSpace(space);
            }

            return result;
        }

        XrResult xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                            uint32_t imageCapacityInput,
                                            uint32_t* imageCountOutput,
                                            XrSwapchainImageBaseHeader* images) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrEnumerateSwapchainImages",
                              TLPArg(swapchain, "Swapchain"),
                              TLArg(imageCapacityInput, "ImageCapacityInput"));

            const XrResult result =
                OpenXrApi::xrEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
            if (m_metroSwapchainIndices.count(swapchain) && XR_SUCCEEDED(result) &&
                m_metroGfxImageLogs.fetch_add(1) < MetroGfxLogSamples) {
                void* firstImage = nullptr;
                if (images && *imageCountOutput) {
                    if (images[0].type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                        firstImage = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[0].texture;
                    } else if (images[0].type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {
                        firstImage = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)[0].texture;
                    }
                }
                Log("[GFX-NEUTRAL] enumerate swapchain=%p count=%u capacity=%u first_runtime_image=%p "
                    "app_image=runtime_image proxy=0 resource_state_before=unobserved "
                    "resource_state_after=unobserved\n",
                    swapchain, *imageCountOutput, imageCapacityInput, firstImage);
            }
            if (XR_SUCCEEDED(result) && images) {
                auto swapchainIt = m_swapchains.find(swapchain);
                if (swapchainIt != m_swapchains.end()) {
                    auto& swapchainState = swapchainIt->second;

                    // Return the application texture.
                    const auto device = m_subProxySwapchains.count(swapchain) ? m_bisectDevice : m_graphicsDevice;
                    if (device->getApi() == graphics::Api::D3D11) {
                        XrSwapchainImageD3D11KHR* d3dImages = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
                        for (uint32_t i = 0; i < *imageCountOutput; i++) {
                            d3dImages[i].texture = swapchainState.images[i].appTexture->getAs<graphics::D3D11>();
                            TraceLoggingWrite(
                                g_traceProvider, "xrEnumerateSwapchainImages", TLPArg(d3dImages[i].texture, "Image"));
                        }
                    } else if (device->getApi() == graphics::Api::D3D12) {
                        XrSwapchainImageD3D12KHR* d3dImages = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
                        for (uint32_t i = 0; i < *imageCountOutput; i++) {
                            d3dImages[i].texture = swapchainState.images[i].appTexture->getAs<graphics::D3D12>();
                            TraceLoggingWrite(
                                g_traceProvider, "xrEnumerateSwapchainImages", TLPArg(d3dImages[i].texture, "Image"));
                        }
                    } else {
                        throw std::runtime_error("Unsupported graphics runtime");
                    }
                }
            }

            TraceLoggingWrite(
                g_traceProvider, "xrEnumerateSwapchainImages", TLArg(*imageCountOutput, "ImageCountOutput"));

            return result;
        }

        XrResult xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo) override {
            if (waitInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(
                g_traceProvider, "xrWaitSwapchainImage", TLPArg(swapchain, "Swapchain"), TLArg(waitInfo->timeout));

            if (m_metroSwapchainIndices.count(swapchain) || m_subProxySwapchains.count(swapchain)) {
                const XrResult result = OpenXrApi::xrWaitSwapchainImage(swapchain, waitInfo);
                if (auto tracker = m_proxyIndexTrackers.find(swapchain); tracker != m_proxyIndexTrackers.end()) {
                    const auto* target = tracker->second.waitTarget();
                    const uint32_t targetIndex = target ? target->index : UINT32_MAX;
                    const bool waitedSuccessfully = result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING;
                    if (waitedSuccessfully) {
                        ++m_proxyIndexSuccessfulWaitCount;
                        if (!tracker->second.waitSucceeded()) invalidateProxyIndex(tracker->second.error.c_str());
                    } else if (result == XR_TIMEOUT_EXPIRED) {
                        ++m_proxyIndexTimeoutCount;
                        if (!target) invalidateProxyIndex("timeout_without_unwaited_image");
                    } else if (XR_FAILED(result)) {
                        invalidateProxyIndex("downstream_wait_failed");
                    }
                    if (m_proxyIndexWaitLogs++ < 48) {
                        Log("[PROXY-INDEX] event=wait swapchain=%p wait_target_index=%u wait_result=%s "
                            "waited_successfully=%u queue_depth=%zu queue=%s\n", swapchain, targetIndex,
                            xr::ToCString(result), waitedSuccessfully, tracker->second.queue.size(),
                            tracker->second.state().c_str());
                    }
                }
                if (m_metroGfxWaitLogs.fetch_add(1) < MetroGfxLogSamples) {
                    Log("[GFX-NEUTRAL] wait swapchain=%p timeout_requested=%lld timeout_forwarded=%lld "
                        "result=%s toolkit_queue_wait=0 toolkit_fence=0\n",
                        swapchain, static_cast<long long>(waitInfo->timeout),
                        static_cast<long long>(waitInfo->timeout), xr::ToCString(result));
                }
                return result;
            }

            // We remove the timeout causing issues with OpenComposite.
            XrSwapchainImageWaitInfo chainWaitInfo = *waitInfo;
            chainWaitInfo.timeout = XR_INFINITE_DURATION;
            return OpenXrApi::xrWaitSwapchainImage(swapchain, &chainWaitInfo);
        }

        XrResult xrAcquireSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageAcquireInfo* acquireInfo,
                                         uint32_t* index) override {
            if (acquireInfo && acquireInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider, "xrAcquireSwapchainImage", TLPArg(swapchain, "Swapchain"));

            auto metroSwapchainIt = m_metroSwapchainIndices.find(swapchain);
            if (metroSwapchainIt != m_metroSwapchainIndices.end()) {
                if (m_subPendingDirectRelease.count(swapchain) && m_subPendingDirectRelease[swapchain]) {
                    CHECK_XRCMD(releaseSubDirectImage(swapchain, "acquire"));
                }
                const XrResult result = OpenXrApi::xrAcquireSwapchainImage(swapchain, acquireInfo, index);
                if (XR_SUCCEEDED(result)) {
                    metroSwapchainIt->second = *index;
                }
                if (m_metroGfxAcquireLogs.fetch_add(1) < MetroGfxLogSamples) {
                    Log("[GFX-NEUTRAL] acquire swapchain=%p index=%u result=%s "
                        "frame_analyzer=0 interceptor=0 debug_workload=0\n",
                        swapchain, XR_SUCCEEDED(result) ? *index : UINT32_MAX, xr::ToCString(result));
                }
                return result;
            }

            auto swapchainIt = m_swapchains.find(swapchain);
            if (swapchainIt != m_swapchains.end()) {
                if (m_frameAnalyzer) {
                    m_frameAnalyzer->onAcquireSwapchain(swapchain);
                }

                // Perform the release now in case it was delayed.
                if (swapchainIt->second.delayedRelease) {
                    TraceLoggingWrite(g_traceProvider, "ForcedSwapchainRelease", TLPArg(swapchain, "Swapchain"));
                    if (m_subProxySwapchains.count(swapchain)) {
                        CHECK_XRCMD(releaseSubProxyImage(swapchain, "acquire"));
                    } else {
                        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, nullptr};
                        swapchainIt->second.delayedRelease = false;
                        CHECK_XRCMD(OpenXrApi::xrReleaseSwapchainImage(swapchain, &releaseInfo));
                    }
                }
            }

            const XrResult result = OpenXrApi::xrAcquireSwapchainImage(swapchain, acquireInfo, index);
            if (XR_SUCCEEDED(result)) {
                // Record the index so we know which texture to use in xrEndFrame().
                if (swapchainIt != m_swapchains.end()) {
                    swapchainIt->second.acquiredImageIndex = *index;
                    if (auto tracker = m_proxyIndexTrackers.find(swapchain); tracker != m_proxyIndexTrackers.end()) {
                        ++m_proxyIndexAcquireCount;
                        if (!tracker->second.acquire(*index, static_cast<uint32_t>(swapchainIt->second.images.size())))
                            invalidateProxyIndex(tracker->second.error.c_str());
                        if (m_proxyIndexAcquireLogs++ < 48) {
                            Log("[PROXY-INDEX] event=acquire swapchain=%p returned_index=%u queue_depth=%zu "
                                "queue=%s\n", swapchain, *index, tracker->second.queue.size(),
                                tracker->second.state().c_str());
                        }
                    }
                    if (m_proxyUsageEnabled) {
                        logProxyUsageAcquire(swapchain, *index);
                    }
                    if (m_proxyTimingEnabled && m_subProxySwapchains.count(swapchain) &&
                        m_proxyTimingLogs.fetch_add(1) < 192) {
                        const auto& image = swapchainIt->second.images.at(*index);
                        Log("[PROXY-TIMING] mode=%u event=acquire frame=%llu swapchain=%p kind=%s "
                            "runtime_index=%u proxy_index=%u proxy_resource=%p runtime_resource=%p\n",
                            m_proxyTimingMode, static_cast<unsigned long long>(m_proxyTimingFrame), swapchain,
                            m_subProxySwapchains.at(swapchain) ? "color" : "depth", *index, *index,
                            image.appTexture->getNativePtr(), image.runtimeTexture->getNativePtr());
                    }
                    if (m_subProxySwapchains.count(swapchain) &&
                        m_metroGfxAcquireLogs.fetch_add(1) < MetroGfxLogSamples) {
                        Log("[GFX-SUBBISECT] app=%s submode=%u acquire swapchain=%p "
                            "runtime_index=%u proxy_index=%u proxy=%u\n",
                            m_applicationName.c_str(), m_gfxSubBisectMode, swapchain, *index, *index,
                            m_subProxySwapchains.at(swapchain));
                    }
                }

                TraceLoggingWrite(g_traceProvider, "xrAcquireSwapchainImage", TLArg(*index, "Index"));

                // Arbitrary location to simulate workload.
                if (m_graphicsDevice) {
                    m_graphicsDevice->executeDebugWorkload();
                }
            } else if (XR_FAILED(result) && m_proxyIndexTrackers.count(swapchain)) {
                invalidateProxyIndex("downstream_acquire_failed");
            }

            return result;
        }

        XrResult xrReleaseSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageReleaseInfo* releaseInfo) override {
            if (releaseInfo && releaseInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider, "xrReleaseSwapchainImage", TLPArg(swapchain, "Swapchain"));

            auto metroSwapchainIt = m_metroSwapchainIndices.find(swapchain);
            if (metroSwapchainIt != m_metroSwapchainIndices.end()) {
                const uint32_t releasedIndex = metroSwapchainIt->second;
                if (m_subPendingDirectRelease.count(swapchain)) {
                    m_subPendingDirectRelease[swapchain] = true;
                    if (m_subReleaseLogs.fetch_add(1) < MetroGfxLogSamples) {
                        Log("[GFX-SUBBISECT] app=%s submode=1 release_requested swapchain=%p runtime_index=%u "
                            "downstream_release=deferred_to_endFrame\n",
                            m_applicationName.c_str(), swapchain, releasedIndex);
                    }
                    return XR_SUCCESS;
                }
                const XrResult result = OpenXrApi::xrReleaseSwapchainImage(swapchain, releaseInfo);
                if (XR_SUCCEEDED(result)) {
                    metroSwapchainIt->second = UINT32_MAX;
                }
                if (m_metroGfxReleaseLogs.fetch_add(1) < MetroGfxLogSamples) {
                    Log("[GFX-NEUTRAL] release swapchain=%p index=%u result=%s "
                        "delayed=0 app_to_runtime_copy=0 toolkit_flush=0 toolkit_fence=0\n",
                        swapchain, releasedIndex, xr::ToCString(result));
                }
                return result;
            }

            auto swapchainIt = m_swapchains.find(swapchain);
            if (swapchainIt != m_swapchains.end()) {
                if (m_subProxySwapchains.count(swapchain)) {
                    if (m_proxyUsageEnabled) {
                        if (swapchainIt->second.delayedRelease) {
                            return XR_ERROR_CALL_ORDER_INVALID;
                        }
                        const auto indexTracker = m_proxyIndexTrackers.find(swapchain);
                        const uint32_t legacyIndex = indexTracker == m_proxyIndexTrackers.end() ?
                            swapchainIt->second.acquiredImageIndex : indexTracker->second.lastAcquiredIndex;
                        uint32_t selectedCopyIndex = legacyIndex;
                        uint32_t fifoIndex = UINT32_MAX;
                        uint32_t oldestWaitedIndex = UINT32_MAX;
                        size_t queueDepthBefore = 0;
                        bool validIndex = false;
                        if (indexTracker != m_proxyIndexTrackers.end()) {
                            auto& tracker = indexTracker->second;
                            queueDepthBefore = tracker.queue.size();
                            const auto* oldest = tracker.releaseTarget();
                            fifoIndex = oldest ? oldest->index : UINT32_MAX;
                            for (const auto& entry : tracker.queue) {
                                if (entry.acquired && entry.successfullyWaited) {
                                    oldestWaitedIndex = entry.index;
                                    break;
                                }
                            }
                            validIndex = tracker.canRelease() && tracker.error.empty();
                            if (!validIndex) invalidateProxyIndex(tracker.error.c_str());
                            if (validIndex && legacyIndex != fifoIndex) ++m_proxyIndexLegacyMismatchCount;
                            if (validIndex) selectedCopyIndex = tracker.selectedCopyIndex(m_proxyIndexMode);
                            if (m_proxyIndexMode == 1) {
                                if (!validIndex) {
                                    logProxyIndexRelease(swapchain, fifoIndex, oldestWaitedIndex,
                                                         UINT32_MAX, legacyIndex, false,
                                                         XR_ERROR_CALL_ORDER_INVALID, queueDepthBefore, tracker);
                                    return XR_ERROR_CALL_ORDER_INVALID;
                                }
                                // Reuse the existing copy/release helper unchanged; only select its image.
                                swapchainIt->second.acquiredImageIndex = selectedCopyIndex;
                            }
                        }
                        const bool copiedNow = m_subProxySwapchains.at(swapchain) && !m_subProxyCopyCompleted[swapchain];
                        const auto result = releaseSubProxyImage(swapchain, "release", releaseInfo);
                        // All resource queries, comparison and logs are AFTER downstream release.
                        logProxyUsageRelease(swapchain, result);
                        if (m_proxyScopeEnabled) logProxyScopeRelease(swapchain, result, copiedNow);
                        // Existing observers inspect acquiredImageIndex for the image just copied.
                        // Restore the last acquire once those post-release observers have run.
                        if (indexTracker != m_proxyIndexTrackers.end() && m_proxyIndexMode == 1)
                            swapchainIt->second.acquiredImageIndex = legacyIndex;
                        if (indexTracker != m_proxyIndexTrackers.end()) {
                            auto& tracker = indexTracker->second;
                            if (validIndex && XR_FAILED(result))
                                invalidateProxyIndex("downstream_release_failed");
                            if (XR_SUCCEEDED(result) && validIndex) {
                                ++m_proxyIndexReleaseCount;
                                if (!tracker.releaseSucceeded()) invalidateProxyIndex(tracker.error.c_str());
                            }
                            logProxyIndexRelease(swapchain, fifoIndex, oldestWaitedIndex,
                                selectedCopyIndex, legacyIndex, validIndex,
                                result, queueDepthBefore, tracker);
                        }
                        return result;
                    }
                    if (m_proxyTimingEnabled) {
                        const bool hasProxy = m_subProxySwapchains.at(swapchain);
                        if (swapchainIt->second.delayedRelease) {
                            return XR_ERROR_CALL_ORDER_INVALID;
                        }
                        if (m_proxyTimingMode == 0) {
                            return releaseSubProxyImage(swapchain, "release", releaseInfo);
                        }
                        if (m_proxyTimingMode == 1 && hasProxy) {
                            copySubProxyImage(swapchain, "release");
                            logProxyTimingCopy(swapchain, "release");
                        }
                        swapchainIt->second.delayedRelease = true;
                        m_proxyTimingPendingOrder.push_back(swapchain);
                        if (m_proxyTimingLogs.fetch_add(1) < 192) {
                            Log("[PROXY-TIMING] app=%s mode=%u event=release_deferred point=release "
                                "swapchain=%p kind=%s image_index=%u copy_done=%u "
                                "downstream_release=endFrame result=XR_SUCCESS\n",
                                m_applicationName.c_str(), m_proxyTimingMode, swapchain,
                                hasProxy ? "color" : "depth", swapchainIt->second.acquiredImageIndex,
                                m_subProxyCopyCompleted[swapchain]);
                        }
                        return XR_SUCCESS;
                    }
                    if (m_gfxSubBisectMode == 2) {
                        return releaseSubProxyImage(swapchain, "release", releaseInfo);
                    }
                    swapchainIt->second.delayedRelease = true;
                    if (m_subReleaseLogs.fetch_add(1) < MetroGfxLogSamples) {
                        Log("[GFX-SUBBISECT] app=%s submode=3 release_requested swapchain=%p "
                            "runtime_index=%u proxy_index=%u downstream_release=deferred_to_endFrame\n",
                            m_applicationName.c_str(), swapchain, swapchainIt->second.acquiredImageIndex,
                            swapchainIt->second.acquiredImageIndex);
                    }
                    return XR_SUCCESS;
                }
                if (m_frameAnalyzer) {
                    m_frameAnalyzer->onReleaseSwapchain(swapchain);
                }

                // Perform a delayed release: we still need to write to the swapchain in our xrEndFrame()!
                swapchainIt->second.delayedRelease = true;
                return XR_SUCCESS;
            }

            return OpenXrApi::xrReleaseSwapchainImage(swapchain, releaseInfo);
        }

        XrResult xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) override {
            TraceLoggingWrite(g_traceProvider, "xrPollEvent", TLPArg(instance, "Instance"));

            m_needVarjoPollEventWorkaround = false;

            if (m_sendInterationProfileEvent && m_vrSession != XR_NULL_HANDLE) {
                XrEventDataInteractionProfileChanged* const buffer =
                    reinterpret_cast<XrEventDataInteractionProfileChanged*>(eventData);
                buffer->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
                buffer->next = nullptr;
                buffer->session = m_vrSession;

                TraceLoggingWrite(g_traceProvider, "InteractionProfileChanged", TLPArg(buffer->session, "Session"));

                m_sendInterationProfileEvent = false;
                return XR_SUCCESS;
            }

            if (m_visibilityMaskEventIndex != utilities::ViewCount && m_vrSession != XR_NULL_HANDLE) {
                XrEventDataVisibilityMaskChangedKHR* const buffer =
                    reinterpret_cast<XrEventDataVisibilityMaskChangedKHR*>(eventData);
                buffer->type = XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR;
                buffer->next = nullptr;
                buffer->session = m_vrSession;
                buffer->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                buffer->viewIndex = m_visibilityMaskEventIndex++;

                TraceLoggingWrite(g_traceProvider,
                                  "VisibilityMaskChanged",
                                  TLPArg(buffer->session, "Session"),
                                  TLArg(xr::ToCString(buffer->viewConfigurationType), "ViewConfigurationType"),
                                  TLArg(buffer->viewIndex, "ViewIndex"));
                Log("Send XrEventDataVisibilityMaskChangedKHR event for view %u\n", buffer->viewIndex);

                return XR_SUCCESS;
            }

            return OpenXrApi::xrPollEvent(instance, eventData);
        }

        XrResult xrGetCurrentInteractionProfile(XrSession session,
                                                XrPath topLevelUserPath,
                                                XrInteractionProfileState* interactionProfile) override {
            if (interactionProfile->type != XR_TYPE_INTERACTION_PROFILE_STATE) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrGetCurrentInteractionProfile",
                              TLPArg(session, "Session"),
                              TLArg(getPath(topLevelUserPath).c_str(), "TopLevelUserPath"));

            std::string path = topLevelUserPath != XR_NULL_PATH ? getPath(topLevelUserPath) : "";
            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (m_handTracker && isVrSession(session) &&
                (path.empty() || path == "/user/hand/left" || path == "/user/hand/right") &&
                interactionProfile->type == XR_TYPE_INTERACTION_PROFILE_STATE) {
                // Return our emulated interaction profile for the hands.
                interactionProfile->interactionProfile = m_handTracker->getInteractionProfile();
                result = XR_SUCCESS;
            } else {
                result = OpenXrApi::xrGetCurrentInteractionProfile(session, topLevelUserPath, interactionProfile);
            }

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider,
                                  "xrGetCurrentInteractionProfile",
                                  TLArg(getPath(interactionProfile->interactionProfile).c_str(), "InteractionProfile"));
            }

            return result;
        }

        XrResult xrGetVisibilityMaskKHR(XrSession session,
                                        XrViewConfigurationType viewConfigurationType,
                                        uint32_t viewIndex,
                                        XrVisibilityMaskTypeKHR visibilityMaskType,
                                        XrVisibilityMaskKHR* visibilityMask) override {
            if (visibilityMask->type != XR_TYPE_VISIBILITY_MASK_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrGetVisibilityMaskKHR",
                              TLPArg(session, "Session"),
                              TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"),
                              TLArg(viewIndex, "ViewIndex"),
                              TLArg(xr::ToCString(visibilityMaskType), "VisibilityMaskType"),
                              TLArg(visibilityMask->vertexCapacityInput, "VertexCapacityInput"),
                              TLArg(visibilityMask->indexCapacityInput, "IndexCapacityInput"));

            bool useFullMask = false;
            bool useEmptyMask = false;

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (isVrSession(session)) {
                if (m_configManager->getValue(config::SettingDisableHAM)) {
                    useEmptyMask = visibilityMaskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR;
                    useFullMask = visibilityMaskType == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR;
                }
                switch (m_configManager->getEnumValue<config::BlindEye>(config::SettingBlindEye)) {
                case config::BlindEye::Left:
                    if (viewIndex == 0) {
                        useEmptyMask = visibilityMaskType == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR;
                        useFullMask = visibilityMaskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR;
                    }
                    break;
                case config::BlindEye::Right:
                    if (viewIndex == 1) {
                        useEmptyMask = visibilityMaskType == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR;
                        useFullMask = visibilityMaskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR;
                    }
                    break;

                case config::BlindEye::None:
                default:
                    break;
                }
            }

            if (useFullMask) {
                visibilityMask->vertexCountOutput = 4;
                visibilityMask->indexCountOutput = 6;
                if (visibilityMask->vertexCapacityInput >= visibilityMask->vertexCountOutput &&
                    visibilityMask->indexCapacityInput >= visibilityMask->indexCountOutput) {
                    visibilityMask->vertices[0] = {2, 2};
                    visibilityMask->vertices[1] = {-2, 2};
                    visibilityMask->vertices[2] = {-2, -2};
                    visibilityMask->vertices[3] = {2, -2};
                    visibilityMask->indices[0] = 0;
                    visibilityMask->indices[1] = 1;
                    visibilityMask->indices[2] = 2;
                    visibilityMask->indices[3] = 2;
                    visibilityMask->indices[4] = 3;
                    visibilityMask->indices[5] = 0;
                }
                result = XR_SUCCESS;
            } else if (useEmptyMask) {
                // TODO: Not great, but we must return something apparently. No vertices/indices does not seem to work
                // for all apps (bad two-call idiom)?
                visibilityMask->vertexCountOutput = 1;
                visibilityMask->indexCountOutput = 3;
                if (visibilityMask->vertexCapacityInput >= visibilityMask->vertexCountOutput &&
                    visibilityMask->indexCapacityInput >= visibilityMask->indexCountOutput) {
                    visibilityMask->vertices[0] = {0, 0};
                    visibilityMask->indices[0] = 0;
                    visibilityMask->indices[1] = 0;
                    visibilityMask->indices[2] = 0;
                }
                result = XR_SUCCESS;
            } else {
                result = OpenXrApi::xrGetVisibilityMaskKHR(
                    session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
            }

            if (XR_SUCCEEDED(result)) {
                if (visibilityMask->type != XR_TYPE_VISIBILITY_MASK_KHR) {
                    return XR_ERROR_VALIDATION_FAILURE;
                }

                TraceLoggingWrite(g_traceProvider,
                                  "xrGetVisibilityMaskKHR",
                                  TLArg(visibilityMask->vertexCountOutput, "VertexCountOutput"),
                                  TLArg(visibilityMask->indexCountOutput, "IndexCountOutput"));
            }

            return result;
        }

        XrResult xrLocateViews(XrSession session,
                               const XrViewLocateInfo* viewLocateInfo,
                               XrViewState* viewState,
                               uint32_t viewCapacityInput,
                               uint32_t* viewCountOutput,
                               XrView* views) override {
            if (viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO || viewState->type != XR_TYPE_VIEW_STATE) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrLocateViews",
                              TLPArg(session, "Session"),
                              TLArg(xr::ToCString(viewLocateInfo->viewConfigurationType), "ViewConfigurationType"),
                              TLArg(viewLocateInfo->displayTime, "DisplayTime"),
                              TLPArg(viewLocateInfo->space, "Space"),
                              TLArg(viewCapacityInput, "ViewCapacityInput"));

            const XrResult result =
                OpenXrApi::xrLocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
            if (XR_SUCCEEDED(result) && isVrSession(session) &&
                viewLocateInfo->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO &&
                viewCapacityInput) {
                assert(*viewCountOutput == utilities::ViewCount);
                using namespace DirectX;

                const XrFovf originalFov[utilities::ViewCount] = {views[0].fov, views[1].fov};
                if (!m_cropCalibrationIdentity.empty() && !m_cropCalibrationChecked) {
                    float ratioWidth = 0.f, ratioHeight = 0.f;
                    if (GetFovRatios(originalFov[0], originalFov[0], ratioWidth, ratioHeight) &&
                        GetFovRatios(originalFov[1], originalFov[1], ratioWidth, ratioHeight)) {
                        m_cropCalibrationChecked = true;
                        for (uint32_t eye = 0; eye < utilities::ViewCount; eye++) {
                            Log("[FOV-CROP] calibration observed eye=%u original=%s\n", eye,
                                xr::ToString(originalFov[eye]).c_str());
                        }
                        bool changed = !m_cropCacheHit;
                        if (m_cropCacheHit) {
                            for (uint32_t eye = 0; eye < utilities::ViewCount; eye++) {
                                const float observed[] = {originalFov[eye].angleLeft, originalFov[eye].angleRight,
                                                          originalFov[eye].angleUp, originalFov[eye].angleDown};
                                const float cached[] = {m_cropOriginalFov[eye].angleLeft,
                                                        m_cropOriginalFov[eye].angleRight,
                                                        m_cropOriginalFov[eye].angleUp,
                                                        m_cropOriginalFov[eye].angleDown};
                                for (uint32_t angle = 0; angle < 4; angle++) {
                                    changed |= std::abs(observed[angle] - cached[angle]) > 0.001f;
                                }
                            }
                        }
                        if (changed) {
                            if (WriteFovCalibration(m_cropCalibrationPath, m_cropCalibrationIdentity, originalFov)) {
                                Log("[FOV-CROP] calibration stored - restart for exact crop; "
                                    "key=\"%s\"\n", m_cropCalibrationIdentity.c_str());
                            } else {
                                Log("[FOV-CROP] calibration write failed file=\"%s\"\n",
                                    m_cropCalibrationPath.string().c_str());
                            }
                        } else {
                            Log("[FOV-CROP] calibration cache verified against xrLocateViews\n");
                        }
                    } else if (!m_cropInvalidFovLogged) {
                        Log("[FOV-CROP] invalid original FOV in xrLocateViews; waiting for valid views\n");
                        m_cropInvalidFovLogged = true;
                    }
                }

                m_posesForFrame[0].pose = views[0].pose;
                m_posesForFrame[1].pose = views[1].pose;

                // Fix Fallout 4 / OpenComposite Decal Issue for WMR
                if (m_overrideParallelProjection) {
                    views[0].pose.orientation.w = views[1].pose.orientation.w;
                    views[0].pose.orientation.x = views[1].pose.orientation.x;
                    views[0].pose.orientation.y = views[1].pose.orientation.y;
                    views[0].pose.orientation.z = views[1].pose.orientation.z;
                }

                // Override the canting angle if requested.
                const int cantOverride = m_configManager->getValue("canting");
                if (cantOverride != 0) {
                    const float angle = (float)(cantOverride * (M_PI / 180));

                    StoreXrPose(&views[0].pose,
                                XMMatrixMultiply(LoadXrPose(views[0].pose),
                                                 XMMatrixRotationRollPitchYaw(0.f, -angle / 2.f, 0.f)));
                    StoreXrPose(&views[1].pose,
                                XMMatrixMultiply(LoadXrPose(views[1].pose),
                                                 XMMatrixRotationRollPitchYaw(0.f, angle / 2.f, 0.f)));
                }

                // Calibrate the projection center for each eye.
                if (m_needCalibrateEyeProjections) {
                    XrViewLocateInfo info = *viewLocateInfo;
                    info.space = m_viewSpace;

                    XrViewState state{XR_TYPE_VIEW_STATE, nullptr};
                    XrView eyeInViewSpace[2] = {{XR_TYPE_VIEW, nullptr}, {XR_TYPE_VIEW, nullptr}};
                    CHECK_XRCMD(OpenXrApi::xrLocateViews(
                        session, &info, &state, viewCapacityInput, viewCountOutput, eyeInViewSpace));

                    if (Pose::IsPoseValid(state.viewStateFlags)) {
                        utilities::GetProjectedGaze(eyeInViewSpace, XrVector3f{0, 0, -1.0f}, m_projCenters);

                        for (uint32_t eye = 0; eye < utilities::ViewCount; eye++) {
                            m_eyeGaze[eye] = m_projCenters[eye];
                        }

                        Log("Projection calibration: %.5f, %.5f | %.5f, %.5f\n",
                            m_projCenters[0].x,
                            m_projCenters[0].y,
                            m_projCenters[1].x,
                            m_projCenters[1].y);

                        if (m_menuHandler) {
                            m_menuHandler->setViewProjectionCenters(m_projCenters[0], m_projCenters[1]);
                        }

                        if (m_variableRateShader) {
                            m_variableRateShader->setViewProjectionCenters(m_projCenters[0], m_projCenters[1]);
                        }

                        m_needCalibrateEyeProjections = false;
                    }
                }

                const auto vec = views[1].pose.position - views[0].pose.position;
                const auto ipd = Length(vec);

                // Override the ICD if requested.
                const int icdOverride = m_configManager->getValue(config::SettingICD);
                if (icdOverride != 1000) {
                    const float icd = (ipd * 1000) / std::max(icdOverride, 1);
                    const auto center = views[0].pose.position + (vec * 0.5f);
                    const auto offset = Normalize(vec) * (icd * 0.5f);
                    views[0].pose.position = center - offset;
                    views[1].pose.position = center + offset;
                    m_stats.icd = icd;

                } else {
                    m_stats.icd = ipd;
                }

                // Override the FOV if requested.
                if ((m_cropActive ? 0 : m_configManager->getValue(config::SettingFOVType)) == 0) {
                    const auto fovOverride = m_cropActive ? m_cropFovPercent :
                                                           m_configManager->getValue(config::SettingFOV);
                    views[0].fov = ScaleSimpleFov(views[0].fov, fovOverride);
                    views[1].fov = ScaleSimpleFov(views[1].fov, fovOverride);
                } else {
                    // XrFovF layout is: L,R,U,D
                    const auto fov1 = XMINT4(m_configManager->getValue(config::SettingFOVLeftLeft),
                                             m_configManager->getValue(config::SettingFOVLeftRight),
                                             m_configManager->getValue(config::SettingFOVUp),
                                             m_configManager->getValue(config::SettingFOVDown));

                    const auto fov2 = XMINT4(m_configManager->getValue(config::SettingFOVRightLeft),
                                             m_configManager->getValue(config::SettingFOVRightRight),
                                             fov1.z,
                                             fov1.w);

                    StoreXrFov(&views[0].fov, LoadXrFov(views[0].fov) * XMLoadSInt4(&fov1) * XMVectorReplicate(0.01f));
                    StoreXrFov(&views[1].fov, LoadXrFov(views[1].fov) * XMLoadSInt4(&fov2) * XMVectorReplicate(0.01f));
                }

                StoreXrFov(&m_stats.fov[0], ConvertToDegrees(views[0].fov));
                StoreXrFov(&m_stats.fov[1], ConvertToDegrees(views[1].fov));

                m_posesForFrame[0].fov = views[0].fov;
                m_posesForFrame[1].fov = views[1].fov;

                if (!m_cropFovLogged) {
                    for (uint32_t eye = 0; eye < utilities::ViewCount; eye++) {
                        const auto& raw = originalFov[eye];
                        const auto& modified = views[eye].fov;
                        float exactWidthRatio = 0.f, exactHeightRatio = 0.f;
                        const bool validRatios = GetFovRatios(raw, modified, exactWidthRatio, exactHeightRatio);
                        const float appliedWidthRatio = m_cropActive
                                                            ? static_cast<float>(m_cropRecommendedWidth[eye]) /
                                                                  m_runtimeRecommendedWidth[eye]
                                                            : 1.f;
                        const float appliedHeightRatio = m_cropActive
                                                             ? static_cast<float>(m_cropRecommendedHeight[eye]) /
                                                                   m_runtimeRecommendedHeight[eye]
                                                             : 1.f;
                        Log("[FOV-CROP] xrLocateViews session=%p eye=%u original=%s modified=%s "
                            "validRatios=%u exactWidthRatio=%.6f exactHeightRatio=%.6f "
                            "appliedWidthRatio=%.6f appliedHeightRatio=%.6f linearApprox=%.4f mode=%s\n",
                            session, eye, xr::ToString(raw).c_str(), xr::ToString(modified).c_str(),
                            validRatios, exactWidthRatio, exactHeightRatio, appliedWidthRatio, appliedHeightRatio,
                            m_cropActive ? m_cropFovPercent * 0.01f : 1.f,
                            m_cropActive ? (m_cropExact ? "exact" : "linear_fallback") : "inactive");
                    }
                    m_cropFovLogged = true;
                }

                // Apply zoom if requested.
                const auto zoom = m_configManager->getValue(config::SettingZoom);
                if (zoom != 10) {
                    StoreXrFov(&views[0].fov, LoadXrFov(views[0].fov) * XMVectorReplicate(1.f / (zoom * 0.1f)));
                    StoreXrFov(&views[1].fov, LoadXrFov(views[1].fov) * XMVectorReplicate(1.f / (zoom * 0.1f)));
                }

                TraceLoggingWrite(g_traceProvider,
                                  "xrLocateViews",
                                  TLArg(*viewCountOutput, "ViewCountOutput"),
                                  TLArg(viewState->viewStateFlags, "ViewStateFlags"),
                                  TLArg(xr::ToString(views[0].pose).c_str(), "LeftPose"),
                                  TLArg(xr::ToString(views[0].fov).c_str(), "LeftFov"),
                                  TLArg(xr::ToString(views[1].pose).c_str(), "RightPose"),
                                  TLArg(xr::ToString(views[1].fov).c_str(), "RightFov"));
            }

            return result;
        }

        XrResult xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) override {
            if (location->type != XR_TYPE_SPACE_LOCATION) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrLocateSpace",
                              TLPArg(space, "Space"),
                              TLPArg(baseSpace, "BaseSpace"),
                              TLArg(time, "Time"));

            if (m_handTracker && m_vrSession != XR_NULL_HANDLE && location->type == XR_TYPE_SPACE_LOCATION) {
                m_performanceCounters.handTrackingTimer->start();
                if (m_handTracker->locate(space, baseSpace, time, getTimeNow(), *location)) {
                    m_performanceCounters.handTrackingTimer->stop();
                    m_stats.handTrackingCpuTimeUs += m_performanceCounters.handTrackingTimer->query();

                    TraceLoggingWrite(g_traceProvider,
                                      "xrLocateSpace",
                                      TLArg(location->locationFlags, "LocationFlags"),
                                      TLArg(xr::ToString(location->pose).c_str(), "Pose"));

                    return XR_SUCCESS;
                }
            }

            return OpenXrApi::xrLocateSpace(space, baseSpace, time, location);
        }

        XrResult xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) override {
            if (syncInfo->type != XR_TYPE_ACTIONS_SYNC_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider, "xrSyncActions", TLPArg(session, "Session"));
            for (uint32_t i = 0; i < syncInfo->countActiveActionSets; i++) {
                TraceLoggingWrite(g_traceProvider,
                                  "xrSyncActions",
                                  TLPArg(syncInfo->activeActionSets[i].actionSet, "ActionSet"),
                                  TLArg(getPath(syncInfo->activeActionSets[i].subactionPath).c_str(), "SubactionPath"));
            }

            XrActionsSyncInfo chainSyncInfo = *syncInfo;
            std::vector<XrActiveActionSet> newActiveActionSets;
            if (m_eyeTracker && isVrSession(session)) {
                const auto eyeTrackerActionSet = m_eyeTracker->getActionSet();
                if (eyeTrackerActionSet != XR_NULL_HANDLE && (!m_isOpenComposite || m_isActionSetAttached)) {
                    newActiveActionSets.resize(chainSyncInfo.countActiveActionSets + 1);
                    memcpy(newActiveActionSets.data(),
                           chainSyncInfo.activeActionSets,
                           chainSyncInfo.countActiveActionSets * sizeof(XrActiveActionSet));
                    uint32_t nextActionSetSlot = chainSyncInfo.countActiveActionSets;

                    newActiveActionSets[nextActionSetSlot].actionSet = eyeTrackerActionSet;
                    newActiveActionSets[nextActionSetSlot++].subactionPath = XR_NULL_PATH;

                    chainSyncInfo.activeActionSets = newActiveActionSets.data();
                    chainSyncInfo.countActiveActionSets = nextActionSetSlot;
                }
            }

            const XrResult result =
                chainSyncInfo.countActiveActionSets ? OpenXrApi::xrSyncActions(session, &chainSyncInfo) : XR_SUCCESS;
            if (XR_FAILED(result) && result != m_lastSyncError) {
                DiagnosticLog("xrSyncActions failure session=%p result=%s forwarded_count=%u eye_included=%u",
                              session, xr::ToCString(result),
                              chainSyncInfo.countActiveActionSets,
                              chainSyncInfo.countActiveActionSets != syncInfo->countActiveActionSets);
            }
            m_lastSyncError = result;
            if (XR_SUCCEEDED(result) && isVrSession(session) &&
                chainSyncInfo.countActiveActionSets != syncInfo->countActiveActionSets && !m_isEyeActionSetSynced) {
                m_isEyeActionSetSynced = true;
                if (m_isOpenComposite) {
                    m_eyeTracker->setActionSetReady(true);
                }
                DiagnosticLog("eye first_sync session=%p result=%s set=%p",
                              session, xr::ToCString(result), m_eyeTracker->getActionSet());
                DiagnosticLog("eye_set_ready session=%p attached=%u sync_seen=1", session, m_isActionSetAttached);
            }
            if (XR_SUCCEEDED(result) && m_handTracker && isVrSession(session)) {
                m_performanceCounters.handTrackingTimer->start();

                m_handTracker->sync(m_begunFrameTime, getTimeNow(), *syncInfo);

                m_performanceCounters.handTrackingTimer->stop();
                m_stats.handTrackingCpuTimeUs += m_performanceCounters.handTrackingTimer->query();
            }

            return result;
        }

        XrResult xrGetActionStateBoolean(XrSession session,
                                         const XrActionStateGetInfo* getInfo,
                                         XrActionStateBoolean* state) override {
            if (getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO || state->type != XR_TYPE_ACTION_STATE_BOOLEAN) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrGetActionStateBoolean",
                              TLPArg(session, "Session"),
                              TLPArg(getInfo->action, "Action"),
                              TLArg(getPath(getInfo->subactionPath).c_str(), "SubactionPath"));

            if (m_handTracker && isVrSession(session) && getInfo->type == XR_TYPE_ACTION_STATE_GET_INFO &&
                state->type == XR_TYPE_ACTION_STATE_BOOLEAN) {
                m_performanceCounters.handTrackingTimer->start();
                if (m_handTracker->getActionState(*getInfo, *state)) {
                    m_performanceCounters.handTrackingTimer->stop();
                    m_stats.handTrackingCpuTimeUs += m_performanceCounters.handTrackingTimer->query();

                    TraceLoggingWrite(g_traceProvider,
                                      "xrGetActionStateBoolean",
                                      TLArg(!!state->isActive, "Active"),
                                      TLArg(!!state->currentState, "CurrentState"),
                                      TLArg(!!state->changedSinceLastSync, "ChangedSinceLastSync"),
                                      TLArg(state->lastChangeTime, "LastChangeTime"));

                    return XR_SUCCESS;
                }
            }

            return OpenXrApi::xrGetActionStateBoolean(session, getInfo, state);
        }

        XrResult xrGetActionStateFloat(XrSession session,
                                       const XrActionStateGetInfo* getInfo,
                                       XrActionStateFloat* state) override {
            if (getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO || state->type != XR_TYPE_ACTION_STATE_FLOAT) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrGetActionStateFloat",
                              TLPArg(session, "Session"),
                              TLPArg(getInfo->action, "Action"),
                              TLArg(getPath(getInfo->subactionPath).c_str(), "SubactionPath"));

            if (m_handTracker && isVrSession(session) && getInfo->type == XR_TYPE_ACTION_STATE_GET_INFO &&
                state->type == XR_TYPE_ACTION_STATE_FLOAT) {
                m_performanceCounters.handTrackingTimer->start();
                if (m_handTracker->getActionState(*getInfo, *state)) {
                    m_performanceCounters.handTrackingTimer->stop();
                    m_stats.handTrackingCpuTimeUs += m_performanceCounters.handTrackingTimer->query();

                    TraceLoggingWrite(g_traceProvider,
                                      "xrGetActionStateFloat",
                                      TLArg(!!state->isActive, "Active"),
                                      TLArg(state->currentState, "CurrentState"),
                                      TLArg(!!state->changedSinceLastSync, "ChangedSinceLastSync"),
                                      TLArg(state->lastChangeTime, "LastChangeTime"));

                    return XR_SUCCESS;
                }
            }

            return OpenXrApi::xrGetActionStateFloat(session, getInfo, state);
        }

        XrResult xrGetActionStatePose(XrSession session,
                                      const XrActionStateGetInfo* getInfo,
                                      XrActionStatePose* state) override {
            if (getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO || state->type != XR_TYPE_ACTION_STATE_POSE) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrGetActionStatePose",
                              TLPArg(session, "Session"),
                              TLPArg(getInfo->action, "Action"),
                              TLArg(getPath(getInfo->subactionPath).c_str(), "SubactionPath"));

            if (m_handTracker && isVrSession(session) && getInfo->type == XR_TYPE_ACTION_STATE_GET_INFO &&
                state->type == XR_TYPE_ACTION_STATE_POSE) {
                m_performanceCounters.handTrackingTimer->start();
                const std::string fullPath = m_handTracker->getFullPath(getInfo->action, getInfo->subactionPath);
                bool supportedPath = false;
                input::Hand hand = input::Hand::Left;
                if (fullPath == "/user/hand/left/input/grip/pose" || fullPath == "/user/hand/left/input/aim/pose") {
                    supportedPath = true;
                    hand = input::Hand::Left;
                } else if (fullPath == "/user/hand/right/input/grip/pose" ||
                           fullPath == "/user/hand/right/input/aim/pose") {
                    supportedPath = true;
                    hand = input::Hand::Right;
                }
                if (supportedPath && m_handTracker->isHandEnabled(hand)) {
                    state->isActive = m_handTracker->isTrackedRecently(hand);
                    m_performanceCounters.handTrackingTimer->stop();
                    m_stats.handTrackingCpuTimeUs += m_performanceCounters.handTrackingTimer->query();

                    TraceLoggingWrite(g_traceProvider, "xrGetActionStatePose", TLArg(!!state->isActive, "Active"));

                    return XR_SUCCESS;
                }
            }

            return OpenXrApi::xrGetActionStatePose(session, getInfo, state);
        }

        XrResult xrApplyHapticFeedback(XrSession session,
                                       const XrHapticActionInfo* hapticActionInfo,
                                       const XrHapticBaseHeader* hapticFeedback) override {
            if (hapticActionInfo->type != XR_TYPE_HAPTIC_ACTION_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrApplyHapticFeedback",
                              TLPArg(session, "Session"),
                              TLPArg(hapticActionInfo->action, "Action"),
                              TLArg(getPath(hapticActionInfo->subactionPath).c_str(), "SubactionPath"));

            if (m_handTracker && isVrSession(session) && hapticActionInfo->type == XR_TYPE_HAPTIC_ACTION_INFO &&
                hapticFeedback->type == XR_TYPE_HAPTIC_VIBRATION) {
                m_performanceCounters.handTrackingTimer->start();
                const std::string fullPath =
                    m_handTracker->getFullPath(hapticActionInfo->action, hapticActionInfo->subactionPath);
                bool supportedPath = false;
                input::Hand hand = input::Hand::Left;
                if (fullPath == "/user/hand/left/output/haptic") {
                    supportedPath = true;
                    hand = input::Hand::Left;
                } else if (fullPath == "/user/hand/right/output/haptic") {
                    supportedPath = true;
                    hand = input::Hand::Right;
                }
                if (supportedPath) {
                    auto haptics = reinterpret_cast<const XrHapticVibration*>(hapticFeedback);
                    m_handTracker->handleOutput(hand, haptics->frequency ? haptics->frequency : 1, haptics->duration);
                    m_performanceCounters.handTrackingTimer->stop();
                    m_stats.handTrackingCpuTimeUs += m_performanceCounters.handTrackingTimer->query();
                }
            }

            return OpenXrApi::xrApplyHapticFeedback(session, hapticActionInfo, hapticFeedback);
        }

        XrResult xrStopHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo) override {
            if (hapticActionInfo->type != XR_TYPE_HAPTIC_ACTION_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrStopHapticFeedback",
                              TLPArg(session, "Session"),
                              TLPArg(hapticActionInfo->action, "Action"),
                              TLArg(getPath(hapticActionInfo->subactionPath).c_str(), "SubactionPath"));

            if (m_handTracker && isVrSession(session) && hapticActionInfo->type == XR_TYPE_HAPTIC_ACTION_INFO) {
                m_performanceCounters.handTrackingTimer->start();
                const std::string fullPath =
                    m_handTracker->getFullPath(hapticActionInfo->action, hapticActionInfo->subactionPath);
                bool supportedPath = false;
                input::Hand hand = input::Hand::Left;
                if (fullPath == "/user/hand/left/output/haptic") {
                    supportedPath = true;
                    hand = input::Hand::Left;
                } else if (fullPath == "/user/hand/right/output/haptic") {
                    supportedPath = true;
                    hand = input::Hand::Right;
                }
                if (supportedPath) {
                    m_handTracker->handleOutput(hand, NAN, 0);
                    m_performanceCounters.handTrackingTimer->stop();
                    m_stats.handTrackingCpuTimeUs += m_performanceCounters.handTrackingTimer->query();
                }
            }

            return OpenXrApi::xrStopHapticFeedback(session, hapticActionInfo);
        }

        XrResult xrWaitFrame(XrSession session,
                             const XrFrameWaitInfo* frameWaitInfo,
                             XrFrameState* frameState) override {
            if ((frameWaitInfo && frameWaitInfo->type != XR_TYPE_FRAME_WAIT_INFO) ||
                frameState->type != XR_TYPE_FRAME_STATE) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider, "xrWaitFrame", TLPArg(session, "Session"));

            const auto lastFrameWaitTimestamp = m_lastFrameWaitTimestamp;
            if (isVrSession(session)) {
                if (m_graphicsDevice) {
                    m_performanceCounters.appCpuTimer->stop();
                    m_stats.appCpuTimeUs += m_performanceCounters.appCpuTimer->query();
                }

                // Do throttling if needed.
                if (m_isFrameThrottlingPossible) {
                    const auto frameThrottling = m_configManager->getValue(config::SettingFrameThrottling);
                    if (frameThrottling < config::MaxFrameRate) {
                        // TODO: Try to reduce latency by slowing slewing to reduce the predictedDisplayTime.

                        const auto target =
                            m_lastFrameWaitTimestamp +
                            std::chrono::microseconds(1000000 / frameThrottling + m_frameThrottleSleepOffset) -
                            500us /* "running start" */;
                        std::this_thread::sleep_until(target);
                    }
                }
                m_lastFrameWaitTimestamp = std::chrono::steady_clock::now();

                m_performanceCounters.waitCpuTimer->start();
            }

            std::unique_lock lock(m_frameLock);

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (isVrSession(session) && m_asyncWaitPromise.valid()) {
                TraceLoggingWrite(g_traceProvider, "AsyncWaitMode");

                // In Turbo mode, we accept pipelining of exactly one frame.
                if (m_asyncWaitPolled) {
                    TraceLocalActivity(local);

                    // On second frame poll, we must wait.
                    TraceLoggingWriteStart(local, "AsyncWaitNow");
                    m_asyncWaitPromise.wait();
                    TraceLoggingWriteStop(local, "AsyncWaitNow");
                }
                m_asyncWaitPolled = true;

                // In Turbo mode, we don't actually wait, we make up a predicted time.
                std::unique_lock lock(m_asyncWaitLock);
                frameState->predictedDisplayTime =
                    m_asyncWaitCompleted
                        ? m_lastPredictedDisplayTime
                        : (m_lastPredictedDisplayTime + (m_lastFrameWaitTimestamp - lastFrameWaitTimestamp).count());
                frameState->predictedDisplayPeriod = m_lastPredictedDisplayPeriod;
                frameState->shouldRender = XR_TRUE;
                result = XR_SUCCESS;
            } else {
                lock.unlock();
                result = OpenXrApi::xrWaitFrame(session, frameWaitInfo, frameState);
                lock.lock();

                if (XR_SUCCEEDED(result)) {
                    // We must always store those values to properly handle transitions into Turbo Mode.
                    m_lastPredictedDisplayTime = frameState->predictedDisplayTime;
                    m_lastPredictedDisplayPeriod = frameState->predictedDisplayPeriod;
                }
            }
            if (XR_SUCCEEDED(result) && isVrSession(session)) {
                m_performanceCounters.waitCpuTimer->stop();
                m_stats.waitCpuTimeUs += m_performanceCounters.waitCpuTimer->query();

                m_savedFrameTime1 = frameState->predictedDisplayTime;

                // Apply prediction dampening if possible and if needed.
                if (m_hasPerformanceCounterKHR) {
                    const int predictionDampen = m_configManager->getValue(config::SettingPredictionDampen);
                    if (predictionDampen != 100) {
                        // Find the current time.
                        LARGE_INTEGER qpcTimeNow;
                        QueryPerformanceCounter(&qpcTimeNow);

                        XrTime xrTimeNow;
                        CHECK_XRCMD(
                            xrConvertWin32PerformanceCounterToTimeKHR(GetXrInstance(), &qpcTimeNow, &xrTimeNow));

                        XrTime predictionAmount = frameState->predictedDisplayTime - xrTimeNow;
                        if (predictionAmount > 0) {
                            frameState->predictedDisplayTime = xrTimeNow + (predictionDampen * predictionAmount) / 100;
                        }

                        m_stats.predictionTimeUs += predictionAmount;
                    }
                }

                // Per OpenXR spec, the predicted display must increase monotonically.
                frameState->predictedDisplayTime = std::max(frameState->predictedDisplayTime, m_waitedFrameTime + 1);

                // Record the predicted display time.
                m_waitedFrameTime = frameState->predictedDisplayTime;

                if (m_graphicsDevice) {
                    m_performanceCounters.appCpuTimer->start();
                }

                m_stats.isFramePipeliningDetected = m_isInFrame;

                TraceLoggingWrite(g_traceProvider,
                                  "xrWaitFrame",
                                  TLArg(!!frameState->shouldRender, "ShouldRender"),
                                  TLArg(frameState->predictedDisplayTime, "PredictedDisplayTime"),
                                  TLArg(frameState->predictedDisplayPeriod, "PredictedDisplayPeriod"));
            }

            if (XR_SUCCEEDED(result) && isMetroGraphicsSession(session) &&
                m_metroGfxFrameWaitLogs.fetch_add(1) < MetroGfxLogSamples) {
                Log("[GFX-NEUTRAL] frame_wait predictedDisplayTime=%lld period=%lld "
                    "toolkit_graphics_queue=0 toolkit_fence=0\n",
                    static_cast<long long>(frameState->predictedDisplayTime),
                    static_cast<long long>(frameState->predictedDisplayPeriod));
            }

            return result;
        }

        XrResult xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) override {
            if (frameBeginInfo && frameBeginInfo->type != XR_TYPE_FRAME_BEGIN_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider, "xrBeginFrame", TLPArg(session, "Session"));

            std::unique_lock lock(m_frameLock);

            // Release the swapchain images. Some runtimes don't seem to look cross-frame releasing and this can happen
            // when a frame is discarded.
            if (isMetroGraphicsSession(session) && m_gfxSubBisectMode == 1) {
                for (const auto& pending : m_subPendingDirectRelease) {
                    if (pending.second) {
                        CHECK_XRCMD(releaseSubDirectImage(pending.first, "beginFrame"));
                    }
                }
            }
            if (m_proxyTimingEnabled) {
                while (!m_proxyTimingPendingOrder.empty()) {
                    CHECK_XRCMD(releaseSubProxyImage(m_proxyTimingPendingOrder.front(), "beginFrame"));
                }
            }
            for (auto& swapchain : m_swapchains) {
                if (swapchain.second.delayedRelease) {
                    TraceLoggingWrite(g_traceProvider, "ForcedSwapchainRelease", TLPArg(swapchain.first, "Swapchain"));
                    if (m_subProxySwapchains.count(swapchain.first)) {
                        CHECK_XRCMD(releaseSubProxyImage(swapchain.first, "beginFrame"));
                    } else {
                        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        swapchain.second.delayedRelease = false;
                        CHECK_XRCMD(OpenXrApi::xrReleaseSwapchainImage(swapchain.first, &releaseInfo));
                    }
                }
            }

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (isVrSession(session) && m_asyncWaitPromise.valid()) {
                // In turbo mode, we do nothing here.
                TraceLoggingWrite(g_traceProvider, "AsyncWaitMode");
                result = XR_SUCCESS;
            } else {
                result = OpenXrApi::xrBeginFrame(session, frameBeginInfo);
            }
            if (XR_SUCCEEDED(result) && isVrSession(session)) {
                // Record the predicted display time.
                m_begunFrameTime = m_waitedFrameTime;
                m_savedFrameTime2 = m_savedFrameTime1;
                m_isInFrame = true;

                if (m_graphicsDevice) {
                    m_performanceCounters.renderCpuTimer->start();
                    m_stats.appGpuTimeUs +=
                        m_performanceCounters.appGpuTimer[m_performanceCounters.gpuTimerIndex]->query();
                    m_performanceCounters.appGpuTimer[m_performanceCounters.gpuTimerIndex]->start();

                    // With D3D12, we want to make sure the query is enqueued now.
                    if (m_graphicsDevice->getApi() == graphics::Api::D3D12) {
                        m_graphicsDevice->flushContext();
                    }

                    if (m_frameAnalyzer) {
                        m_frameAnalyzer->resetForFrame();
                    }
                }

                if (m_eyeTracker || m_handTracker) {
                    // Force artifical syncing of actions if the app doesn't seem to use actions.
                    if (!m_isActionSetUsed && !m_isOpenComposite) {
                        if (!m_artificialActionsLogged) {
                            DiagnosticLog("xrBeginFrame artificial_actions session=%p attached=%u eye_set=%p",
                                          session, m_isActionSetAttached,
                                          m_eyeTracker ? m_eyeTracker->getActionSet() : XR_NULL_HANDLE);
                            m_artificialActionsLogged = true;
                        }
                        if (m_eyeTracker && m_eyeTracker->getActionSet() != XR_NULL_HANDLE) {
                            if (!m_isActionSetAttached) {
                                XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
                                CHECK_XRCMD(xrAttachSessionActionSets(m_vrSession, &attachInfo));
                            }

                            // The app does not implement controller support, we must sync actions ourselves.

                            // Workaround: the eye tracker on Varjo does not seem to connect unless the app calls
                            // xrPollEvent(). So we make a call here if the app does not call it. A disciplined app
                            // should have called xrPollEvent() by now just to begin the session.
                            if (m_needVarjoPollEventWorkaround) {
                                XrEventDataBuffer buf{XR_TYPE_EVENT_DATA_BUFFER};
                                OpenXrApi::xrPollEvent(GetXrInstance(), &buf);
                            }
                        }

                        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
                        CHECK_XRCMD(xrSyncActions(m_vrSession, &syncInfo));
                    }

                    if (m_eyeTracker) {
                        m_eyeTracker->beginFrame(m_begunFrameTime);
                    }
                }

                if (m_variableRateShader) {
                    m_variableRateShader->beginFrame(m_begunFrameTime);
                }
            }

            if (XR_SUCCEEDED(result) && isMetroGraphicsSession(session) && m_gfxBisectMode == 2) {
                m_bisectDevice->flushContext();
            }
            if (isMetroGraphicsSession(session) && m_metroGfxBeginLogs.fetch_add(1) < MetroGfxLogSamples) {
                Log("[GFX-NEUTRAL] begin result=%s frame_analyzer=0 interceptor=0 "
                    "toolkit_flushContext=%u toolkit_command_lists=%u toolkit_fences=%u\n",
                    xr::ToCString(result), XR_SUCCEEDED(result) && m_gfxBisectMode == 2,
                    m_gfxBisectMode > 0, m_gfxBisectMode > 0);
            }

            return result;
        }

        void updateStatisticsForFrame() {
            const auto now = std::chrono::steady_clock::now();
            const auto numFrames = ++m_performanceCounters.numFrames;

            if (m_graphicsDevice) {
                m_stats.numBiasedSamplers = m_graphicsDevice->getNumBiasedSamplersThisFrame();
            }

            if (m_variableRateShader) {
                m_stats.actualRenderWidth = m_variableRateShader->getActualRenderWidth();
            }

            if (m_frameAnalyzer) {
                m_stats.frameAnalyzerHeuristic = m_frameAnalyzer->getCurrentHeuristic();
            }

            if (m_configManager->hasChanged(config::SettingRecordStats)) {
                if (m_configManager->getValue(config::SettingRecordStats)) {
                    const std::time_t now = std::time(nullptr);
                    char buf[1024];
                    std::strftime(buf, sizeof(buf), "stats_%Y%m%d_%H%M%S", std::localtime(&now));
                    std::string logFile = (localAppData / "stats" / (std::string(buf) + ".csv")).string();
                    m_logStats.open(logFile, std::ios_base::ate);

                    // Write headers.
                    m_logStats << "time,FPS,appCPU (us),renderCPU (us),appGPU (us),VRAM (MB),VRAM (%)\n";
                } else {
                    m_logStats.close();
                }
            }

            const bool highRate = m_configManager->getValue(config::SettingHighRateStats);
            if ((now - m_performanceCounters.lastWindowStart) >= (highRate ? 100ms : 1s)) {
                const auto duration = now - m_performanceCounters.lastWindowStart;
                m_performanceCounters.numFrames = 0;
                m_performanceCounters.lastWindowStart = now;

                // TODO: no need to compute these if no menu handler
                // or if menu isn't displaying any stats.

                // Push the last averaged statistics.
                m_stats.appCpuTimeUs /= numFrames;
                m_stats.renderCpuTimeUs /= numFrames;
                m_stats.appGpuTimeUs /= numFrames;
                m_stats.waitCpuTimeUs /= numFrames;
                m_stats.endFrameCpuTimeUs /= numFrames;
                m_stats.processorGpuTimeUs[0] /= numFrames;
                m_stats.processorGpuTimeUs[1] /= numFrames;
                m_stats.overlayCpuTimeUs /= numFrames;
                m_stats.overlayGpuTimeUs /= numFrames;
                m_stats.handTrackingCpuTimeUs /= numFrames;
                m_stats.predictionTimeUs /= numFrames;
                if (highRate) {
                    // We must still do a rolling average for the FPS otherwise the values are all over the place.
                    m_performanceCounters.frameRates.push_front(std::make_pair(duration, numFrames));
                    m_performanceCounters.framesInPeriod += numFrames;
                    m_performanceCounters.timePeriod += duration;
                    while (m_performanceCounters.frameRates.size() > 10) {
                        m_performanceCounters.framesInPeriod -= m_performanceCounters.frameRates.back().second;
                        m_performanceCounters.timePeriod -= m_performanceCounters.frameRates.back().first;
                        m_performanceCounters.frameRates.pop_back();
                    }
                    m_stats.fps =
                        m_performanceCounters.framesInPeriod * (1e9f / m_performanceCounters.timePeriod.count());
                } else {
                    m_stats.fps = static_cast<float>(numFrames);
                    m_performanceCounters.frameRates.clear();
                    m_performanceCounters.framesInPeriod = 0;
                    m_performanceCounters.timePeriod = 0s;
                }

                m_graphicsDevice->getVRAMUsage(m_stats.vramUsedSize, m_stats.vramUsedPercent);

                // When CPU-bound, do not bother giving a (false) GPU time for D3D12
                if (m_graphicsDevice->getApi() == graphics::Api::D3D12 &&
                    m_stats.appCpuTimeUs + 500 > m_stats.appGpuTimeUs) {
                    m_stats.appGpuTimeUs = 0;
                }

                if (m_menuHandler) {
                    m_menuHandler->updateStatistics(m_stats);
                }

                if (m_logStats.is_open()) {
                    const std::time_t now = std::time(nullptr);

                    char buf[1024];
                    size_t offset = std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %z", std::localtime(&now));
                    m_logStats << buf << "," << std::fixed << std::setprecision(1) << m_stats.fps << ","
                               << m_stats.appCpuTimeUs << "," << m_stats.renderCpuTimeUs << "," << m_stats.appGpuTimeUs
                               << "," << m_stats.vramUsedSize / (1024 * 1024) << "," << (int)m_stats.vramUsedPercent
                               << "\n";
                }

                // Start from fresh!
                memset(&m_stats, 0, sizeof(m_stats));
            }

            if (m_handTracker && m_menuHandler) {
                m_menuHandler->updateGesturesState(m_handTracker->getGesturesState());
            }
            if (m_eyeTracker && m_menuHandler) {
                m_menuHandler->updateEyeGazeState(m_eyeTracker->getEyeGazeState());
            }

            for (unsigned int eye = 0; eye < utilities::ViewCount; eye++) {
                m_stats.hasColorBuffer[eye] = m_stats.hasDepthBuffer[eye] = false;
            }
            m_stats.numRenderTargetsWithVRS = 0;
        }

        void updateConfiguration() {
            // Make sure config gets written if needed.
            m_configManager->tick();

            // Forward the motion reprojection locking values to WMR.
            if (m_supportMotionReprojectionLock &&
                (m_configManager->hasChanged(config::SettingMotionReprojection) ||
                 m_configManager->hasChanged(config::SettingMotionReprojectionRate))) {
                const auto motionReprojection =
                    m_configManager->getEnumValue<config::MotionReprojection>(config::SettingMotionReprojection);
                const auto rate = m_configManager->getEnumValue<config::MotionReprojectionRate>(
                    config::SettingMotionReprojectionRate);

                // If motion reprojection is not controlled by us, then make sure the reprojection rate is left to
                // default.
                if (motionReprojection != config::MotionReprojection::On) {
                    utilities::UpdateWindowsMixedRealityReprojectionRate(config::MotionReprojectionRate::Off);
                } else {
                    utilities::UpdateWindowsMixedRealityReprojectionRate(rate);
                }
            }

            // Adjust mip map biasing.
            if ((m_upscaleMode == config::ScalingType::NIS ||
                 m_upscaleMode == config::ScalingType::FSR ||
                 m_upscaleMode == config::ScalingType::CAS
                ) &&
                m_configManager->hasChanged(config::SettingMipMapBias)) {
                m_graphicsDevice->setMipMapBias(
                    m_configManager->getEnumValue<config::MipMapBias>(config::SettingMipMapBias),
                    m_mipMapBiasForUpscaling);
            }

            // Update HAM.
            if (m_configManager->hasChanged(config::SettingDisableHAM) ||
                m_configManager->hasChanged(config::SettingBlindEye)) {
                // Kick off HAM event.
                m_visibilityMaskEventIndex = 0;

                // The app might ignore the HAM events. We acknowledge the config change regardless.
                (void)m_configManager->getValue(config::SettingDisableHAM);
                (void)m_configManager->getEnumValue<config::BlindEye>(config::SettingBlindEye);
            }

            // Check to reload shaders.
            bool reloadShaders = false;
            if (m_configManager->hasChanged(config::SettingReloadShaders)) {
                if (m_configManager->getValue(config::SettingReloadShaders)) {
                    m_configManager->setValue(config::SettingReloadShaders, 0, true);
                    reloadShaders = true;
                }
            }

            // Refresh the configuration.
            if (m_upscaler) {
                if (reloadShaders) {
                    m_upscaler->reload();
                }
                m_upscaler->update();
            }
            if (reloadShaders) {
                m_postProcessor->reload();
            }
            m_postProcessor->update();

            if (m_eyeTracker) {
                m_eyeTracker->update();
            }

            if (m_variableRateShader) {
                m_variableRateShader->update();
            }
        }

        void takeScreenshot(std::shared_ptr<graphics::ITexture> texture,
                            const std::string& suffix,
                            const XrRect2Di& viewport) const {
            // Stamp the overlay/menu if it's active.
            if (m_menuHandler && !isSubBisectOverlaySuppressed()) {
                m_graphicsDevice->setRenderTargets(1, &texture, nullptr, &viewport);
                m_graphicsDevice->beginText(true /* mustKeepOldContent */);
                m_menuHandler->render(texture);
                m_graphicsDevice->flushText();

                m_graphicsDevice->unsetRenderTargets();
            }

            SYSTEMTIME st;
            ::GetLocalTime(&st);

            std::stringstream parameters;
            parameters << '_' << ((st.wYear * 10000u) + (st.wMonth * 100u) + (st.wDay)) << '_'
                       << ((st.wHour * 10000u) + (st.wMinute * 100u) + (st.wSecond));

            if (m_upscaleMode != config::ScalingType::None) {
                // TODO: add a getUpscaleModeName() helper to keep enum and string in sync.
                const auto upscaleName = m_upscaleMode == config::ScalingType::NIS   ? "_NIS_"
                                         : m_upscaleMode == config::ScalingType::FSR ? "_FSR_"
                                         : m_upscaleMode == config::ScalingType::CAS ? "_CAS_"
                                                                                     : "_SCL_";
                parameters << upscaleName << m_settingScaling << "_"
                           << m_configManager->getValue(config::SettingSharpness);
            }

            parameters << "_" << suffix;

            const auto fileFormat =
                m_configManager->getEnumValue<config::ScreenshotFileFormat>(config::SettingScreenshotFileFormat);

            const auto fileExtension = fileFormat == config::ScreenshotFileFormat::DDS   ? ".dds"
                                       : fileFormat == config::ScreenshotFileFormat::JPG ? ".jpg"
                                       : fileFormat == config::ScreenshotFileFormat::BMP ? ".bmp"
                                                                                         : ".png";
            // Using std::filesystem automatically filters out unwanted app name chars.
            std::string sanitizedName = m_applicationName;
            std::replace(sanitizedName.begin(), sanitizedName.end(), '.', '_');
            auto path = localAppData / "screenshots" / (sanitizedName + parameters.str());
            path.replace_extension(fileExtension);

            // Handle VPRT.
            auto info = texture->getInfo();
            if (info.arraySize > 1 || viewport.offset.x || viewport.offset.y || info.width != viewport.extent.width ||
                info.height != viewport.extent.height) {
                const auto srcSlice = (info.arraySize > 1 && suffix == "R") ? 1 : 0;
                info.arraySize = 1;
                info.width = viewport.extent.width;
                info.height = viewport.extent.height;
                auto cropped = m_graphicsDevice->createTexture(info, "Screenshot");
                texture->copyTo(viewport.offset.x, viewport.offset.y, srcSlice, cropped);
                texture = cropped;
            }

            texture->saveToFile(path);
        }

        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            if (frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrEndFrame",
                              TLPArg(session, "Session"),
                              TLArg(frameEndInfo->displayTime, "DisplayTime"),
                              TLArg(xr::ToCString(frameEndInfo->environmentBlendMode), "EnvironmentBlendMode"));

            if (isMetroGraphicsSession(session)) {
                if (m_proxyUsageEnabled) {
                    observeProxyUsageRoles(frameEndInfo);
                }
                // Complete every pending release before the runtime sees the submitted composition layers.
                if (m_gfxSubBisectMode == 1) {
                    for (const auto& pending : m_subPendingDirectRelease) {
                        if (pending.second) {
                            CHECK_XRCMD(releaseSubDirectImage(pending.first, "endFrame"));
                        }
                    }
                } else if (m_proxyTimingEnabled && m_proxyTimingMode > 0) {
                    // Successful releases remove the front entry; failures keep it pending.
                    while (!m_proxyTimingPendingOrder.empty()) {
                        CHECK_XRCMD(releaseSubProxyImage(m_proxyTimingPendingOrder.front(), "endFrame"));
                    }
                } else if (m_gfxSubBisectMode == 3) {
                    for (const auto& state : m_subProxySwapchains) {
                        if (m_swapchains.at(state.first).delayedRelease) {
                            CHECK_XRCMD(releaseSubProxyImage(state.first, "endFrame"));
                        }
                    }
                }
                const uint32_t sample = m_metroGfxEndLogs.fetch_add(1);
                if (m_gfxBisectMode == 2) {
                    m_bisectDevice->flushContext();
                }
                if (sample < MetroGfxLogSamples) {
                    Log("[GFX-NEUTRAL] end sample=%u displayTime=%lld layers=%u "
                        "runtime_swapchains=%s app_to_runtime_copy=%u postprocess=0 "
                        "frame_analyzer=0 interceptor=0 toolkit_flushContext=%u "
                        "toolkit_command_lists=%u toolkit_fences=%u "
                        "resource_state_before=unobserved resource_state_after=unobserved\n",
                        sample, static_cast<long long>(frameEndInfo->displayTime), frameEndInfo->layerCount,
                        m_gfxSubBisectMode >= 2 ? "proxy_color" : "direct",
                        m_gfxSubBisectMode >= 2,
                        m_gfxBisectMode == 2, m_gfxBisectMode > 0, m_gfxBisectMode > 0);
                    for (uint32_t i = 0; frameEndInfo->layers && i < frameEndInfo->layerCount; i++) {
                        if (frameEndInfo->layers[i] &&
                            frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(
                                frameEndInfo->layers[i]);
                            for (uint32_t eye = 0; projection->views && eye < projection->viewCount; eye++) {
                                const auto& view = projection->views[eye];
                                Log("[GFX-NEUTRAL] submit sample=%u layer=%u eye=%u swapchain=%p "
                                    "imageArrayIndex=%u imageRect=%s runtime_handle=%u\n",
                                    sample, i, eye, view.subImage.swapchain,
                                    view.subImage.imageArrayIndex,
                                    xr::ToString(view.subImage.imageRect).c_str(),
                                    m_metroSwapchainIndices.count(view.subImage.swapchain) != 0);
                            }
                        }
                    }
                }
                const auto endFrameEnterTick = GetTickCount64();
                const XrResult result = OpenXrApi::xrEndFrame(session, frameEndInfo);
                if (m_proxyTimingEnabled && m_proxyTimingLogs.fetch_add(1) < 192) {
                    Log("[PROXY-TIMING] mode=%u event=xrEndFrame frame=%llu enter_tick=%llu "
                        "return_tick=%llu pending=%u result=%s\n", m_proxyTimingMode,
                        static_cast<unsigned long long>(m_proxyTimingFrame++),
                        static_cast<unsigned long long>(endFrameEnterTick),
                        static_cast<unsigned long long>(GetTickCount64()),
                        static_cast<unsigned>(m_proxyTimingPendingOrder.size()), xr::ToCString(result));
                }
                if (sample < MetroGfxLogSamples) {
                    Log("[GFX-NEUTRAL] end_result sample=%u result=%s submitted_layers=%u "
                        "extra_copy_or_sync=%u\n", sample, xr::ToCString(result),
                        frameEndInfo->layerCount, m_gfxBisectMode == 2);
                }
                return result;
            }

            if (!isVrSession(session) || !m_graphicsDevice) {
                return OpenXrApi::xrEndFrame(session, frameEndInfo);
            }

            std::unique_lock lock(m_frameLock);

            m_isInFrame = false;

            updateStatisticsForFrame();

            m_performanceCounters.renderCpuTimer->stop();
            m_stats.renderCpuTimeUs += m_performanceCounters.renderCpuTimer->query();
            m_performanceCounters.appGpuTimer[m_performanceCounters.gpuTimerIndex]->stop();

            m_stats.endFrameCpuTimeUs += m_performanceCounters.endFrameCpuTimer->query();
            m_performanceCounters.endFrameCpuTimer->start();

            // Toggle to the next set of GPU timers.
            m_performanceCounters.gpuTimerIndex = (m_performanceCounters.gpuTimerIndex + 1) % (GpuTimerLatency + 1);
            m_graphicsDevice->resolveQueries();

            if (m_frameAnalyzer) {
                m_frameAnalyzer->prepareForEndFrame();
            }

            // TODO: Ensure restoreContext() even on error.
            m_graphicsDevice->blockCallbacks();

            if (m_eyeTracker) {
                m_eyeTracker->endFrame();
            }

            if (m_variableRateShader) {
                m_variableRateShader->endFrame();
                m_variableRateShader->stopCapture();
            }

            m_graphicsDevice->saveContext();

            // Handle inputs.
            if (m_menuHandler && !isSubBisectOverlaySuppressed()) {
                // Defer creating the menu swapchain to avoid issues with OpenComposite double-initialization.
                if (m_menuSwapchain == XR_NULL_HANDLE) {
                    createMenuSwapchain();
                }

                m_menuHandler->handleInput();
            }

            // Prepare the Shaders for rendering.
            updateConfiguration();

            // Unbind all textures from the render targets.
            m_graphicsDevice->unsetRenderTargets();

            std::shared_ptr<graphics::ITexture> textureForOverlay[utilities::ViewCount] = {};
            uint32_t sliceForOverlay[utilities::ViewCount];
            std::shared_ptr<graphics::ITexture> depthForOverlay[utilities::ViewCount] = {};
            xr::math::ViewProjection viewForOverlay[utilities::ViewCount];
            XrRect2Di viewportForOverlay[utilities::ViewCount];
            XrSpace spaceForOverlay = XR_NULL_HANDLE;

            // Because the frame info is passed const, we are going to need to reconstruct a writable version of it
            // to patch the resolution.
            XrFrameEndInfo chainFrameEndInfo = *frameEndInfo;
            std::vector<const XrCompositionLayerBaseHeader*> correctedLayers;

            std::vector<XrCompositionLayerProjection> layerProjectionAllocator;
            std::vector<std::array<XrCompositionLayerProjectionView, 2>> layerProjectionViewsAllocator;
            XrCompositionLayerQuad layerQuadForMenu{XR_TYPE_COMPOSITION_LAYER_QUAD};

            // We must reserve the underlying storage to keep our pointers stable.
            layerProjectionAllocator.reserve(chainFrameEndInfo.layerCount);
            layerProjectionViewsAllocator.reserve(chainFrameEndInfo.layerCount);

            // Apply the processing chain to all the (supported) layers.
            for (uint32_t i = 0; i < chainFrameEndInfo.layerCount; i++) {
                if (chainFrameEndInfo.layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const XrCompositionLayerProjection* proj =
                        reinterpret_cast<const XrCompositionLayerProjection*>(chainFrameEndInfo.layers[i]);

                    TraceLoggingWrite(g_traceProvider,
                                      "xrEndFrame_Layer",
                                      TLArg("Proj", "Type"),
                                      TLArg(proj->layerFlags, "Flags"),
                                      TLPArg(proj->space, "Space"));

                    // To patch the resolution of the layer we need to recreate the whole projection & views
                    // data structures.
                    auto correctedProjectionLayer = &layerProjectionAllocator.emplace_back(*proj);
                    auto correctedProjectionViews = layerProjectionViewsAllocator
                                                        .emplace_back(std::array<XrCompositionLayerProjectionView, 2>(
                                                            {proj->views[0], proj->views[1]}))
                                                        .data();

                    // When using texture arrays, we assume both eyes are submitted from the same swapchain.
                    static_assert(utilities::ViewCount == 2);
                    const bool useTextureArrays =
                        proj->views[0].subImage.swapchain == proj->views[1].subImage.swapchain &&
                        proj->views[0].subImage.imageArrayIndex != proj->views[1].subImage.imageArrayIndex;
                    const bool useDoubleWide = proj->views[0].subImage.swapchain == proj->views[1].subImage.swapchain &&
                                               proj->views[1].subImage.imageRect.offset.x != 0;
                    // TODO: Here we assume that left is always "first" (either slice 0 or "left-most" viewport).

                    if (useTextureArrays || useDoubleWide) {
                        // Assume that we've properly distinguished left/right eyes.
                        m_stats.hasColorBuffer[(int)utilities::Eye::Left] =
                            m_stats.hasColorBuffer[(int)utilities::Eye::Right] = true;
                    }

                    assert(proj->viewCount == utilities::ViewCount);
                    for (uint32_t eye = 0; eye < utilities::ViewCount; eye++) {
                        const XrCompositionLayerProjectionView& view = proj->views[eye];

                        TraceLoggingWrite(g_traceProvider,
                                          "xrEndFrame_View",
                                          TLArg("Proj", "Type"),
                                          TLArg(eye, "Index"),
                                          TLPArg(proj->views[eye].subImage.swapchain, "Swapchain"),
                                          TLArg(proj->views[eye].subImage.imageArrayIndex, "ImageArrayIndex"),
                                          TLArg(xr::ToString(proj->views[eye].subImage.imageRect).c_str(), "ImageRect"),
                                          TLArg(xr::ToString(proj->views[eye].pose).c_str(), "Pose"),
                                          TLArg(xr::ToString(proj->views[eye].fov).c_str(), "Fov"));

                        auto swapchainIt = m_swapchains.find(view.subImage.swapchain);
                        if (swapchainIt == m_swapchains.end()) {
                            throw std::runtime_error("Swapchain is not registered");
                        }
                        auto& swapchainState = swapchainIt->second;
                        auto& swapchainImages = swapchainState.images[swapchainState.acquiredImageIndex];
                        if (m_cropActive && !swapchainState.cropRecommendationLogged[eye]) {
                            const uint32_t expectedWidth = useDoubleWide
                                                               ? m_cropRecommendedWidth[0] + m_cropRecommendedWidth[1]
                                                               : useTextureArrays ? m_displayWidth
                                                                                  : m_cropRecommendedWidth[eye];
                            const uint32_t expectedHeight = useDoubleWide || useTextureArrays
                                                                ? m_displayHeight : m_cropRecommendedHeight[eye];
                            const uint32_t originalWidth = useDoubleWide
                                                               ? m_runtimeRecommendedWidth[0] +
                                                                     m_runtimeRecommendedWidth[1]
                                                               : m_runtimeRecommendedWidth[eye];
                            const bool targetRect =
                                (view.subImage.imageRect.extent.width == m_cropRecommendedWidth[eye] &&
                                 view.subImage.imageRect.extent.height == m_cropRecommendedHeight[eye]) ||
                                (useTextureArrays && view.subImage.imageRect.extent.width == m_displayWidth &&
                                 view.subImage.imageRect.extent.height == m_displayHeight);
                            const bool originalRect =
                                view.subImage.imageRect.extent.width == m_runtimeRecommendedWidth[eye] &&
                                view.subImage.imageRect.extent.height == m_runtimeRecommendedHeight[eye];
                            const bool targetTexture =
                                (swapchainState.requestedWidth == expectedWidth ||
                                 (useDoubleWide && swapchainState.requestedWidth == m_displayWidth * 2)) &&
                                swapchainState.requestedHeight == expectedHeight;
                            const bool originalTexture = swapchainState.requestedWidth == originalWidth &&
                                                         swapchainState.requestedHeight ==
                                                             m_runtimeRecommendedHeight[eye];
                            const char* status = targetRect && targetTexture ? "accepted"
                                                 : originalRect && originalTexture ? "ignored"
                                                                                   : "custom_or_undetermined";
                            Log("[FOV-CROP] %s crop recommendation %s session=%p swapchain=%p eye=%u layout=%s "
                                "arraySize=%u requested=%ux%u imageRect=%s recommended=%ux%u "
                                "sharedMax=%ux%u raw=%ux%u arrayAdequate=%u\n",
                                m_cropExact ? "exact" : "linear_fallback",
                                status, session, view.subImage.swapchain, eye,
                                useTextureArrays ? "texture_array" : useDoubleWide ? "side_by_side" : "separate_swapchain",
                                swapchainState.requestedArraySize, swapchainState.requestedWidth,
                                swapchainState.requestedHeight, xr::ToString(view.subImage.imageRect).c_str(),
                                m_cropRecommendedWidth[eye], m_cropRecommendedHeight[eye],
                                m_displayWidth, m_displayHeight,
                                m_runtimeRecommendedWidth[eye], m_runtimeRecommendedHeight[eye],
                                !useTextureArrays || (swapchainState.requestedWidth >= m_displayWidth &&
                                                      swapchainState.requestedHeight >= m_displayHeight));
                            swapchainState.cropRecommendationLogged[eye] = true;
                        }

                        // Look for the depth buffer.
                        std::shared_ptr<graphics::ITexture> depthBuffer;
                        NearFar nearFar{0.001f, 100.f};
                        const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(view.next);
                        while (entry) {
                            if (entry->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                                const XrCompositionLayerDepthInfoKHR* depth =
                                    reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(entry);

                                TraceLoggingWrite(g_traceProvider,
                                                  "xrEndFrame_View",
                                                  TLArg("Depth", "Type"),
                                                  TLArg(eye, "Index"),
                                                  TLPArg(depth->subImage.swapchain, "Swapchain"),
                                                  TLArg(depth->subImage.imageArrayIndex, "ImageArrayIndex"),
                                                  TLArg(xr::ToString(depth->subImage.imageRect).c_str(), "ImageRect"),
                                                  TLArg(depth->nearZ, "Near"),
                                                  TLArg(depth->farZ, "Far"),
                                                  TLArg(depth->minDepth, "MinDepth"),
                                                  TLArg(depth->maxDepth, "MaxDepth"));

                                // The order of color/depth textures must match.
                                if (depth->subImage.imageArrayIndex == view.subImage.imageArrayIndex) {
                                    auto depthSwapchainIt = m_swapchains.find(depth->subImage.swapchain);
                                    if (depthSwapchainIt == m_swapchains.end()) {
                                        throw std::runtime_error("Swapchain is not registered");
                                    }
                                    auto& depthSwapchainState = depthSwapchainIt->second;

                                    depthBuffer =
                                        depthSwapchainState.images[depthSwapchainState.acquiredImageIndex].appTexture;
                                    nearFar.Near = depth->nearZ;
                                    nearFar.Far = depth->farZ;

                                    m_stats.hasDepthBuffer[eye] = true;
                                }
                                break;
                            }
                            entry = entry->next;
                        }

                        const bool isDepthInverted = nearFar.Far < nearFar.Near;

                        // Now that we know what eye the swapchain is used for, register it.
                        // TODO: We always assume that if texture arrays are used, left eye is texture 0 and right eye
                        // is texture 1. I'm sure this holds in like 99% of the applications, but still not very clean
                        // to assume.
                        if (m_frameAnalyzer && !useTextureArrays && !swapchainState.registeredWithFrameAnalyzer) {
                            for (const auto& image : swapchainState.images) {
                                m_frameAnalyzer->registerColorSwapchainImage(
                                    view.subImage.swapchain, image.appTexture, (utilities::Eye)eye);
                            }
                            swapchainState.registeredWithFrameAnalyzer = true;
                        }

                        // Detect whether the input uses a viewport (VP) or a texture array render target (RT)
                        const bool isVPRT =
                            view.subImage.imageArrayIndex > 0 || view.subImage.imageRect.offset.x ||
                            view.subImage.imageRect.offset.y ||
                            view.subImage.imageRect.extent.width != swapchainImages.appTexture->getInfo().width ||
                            view.subImage.imageRect.extent.height != swapchainImages.appTexture->getInfo().height ||
                            m_configManager->getValue("force_vprt_path");

                        std::shared_ptr<graphics::ITexture> nextInput = swapchainImages.appTexture;
                        std::shared_ptr<graphics::ITexture> finalOutput = swapchainImages.runtimeTexture;

                        float horizontalScaleFactor = 1.f;
                        float verticalScaleFactor = 1.f;
                        uint32_t scaledOutputWidth = view.subImage.imageRect.extent.width;
                        uint32_t scaledOutputHeight = view.subImage.imageRect.extent.height;
                        if (m_upscaleMode == config::ScalingType::NIS 
                            || m_upscaleMode == config::ScalingType::FSR
                            || m_upscaleMode == config::ScalingType::CAS
                        ) {
                            std::tie(horizontalScaleFactor, verticalScaleFactor) =
                                config::GetScalingFactors(m_settingScaling, m_settingAnamorphic);

                            scaledOutputWidth = roundUp(
                                (uint32_t)std::ceil(view.subImage.imageRect.extent.width * horizontalScaleFactor), 2);
                            scaledOutputHeight = roundUp(
                                (uint32_t)std::ceil(view.subImage.imageRect.extent.height * verticalScaleFactor), 2);
                        }

                        // Copy the VPRT app input into an intermediate buffer if needed.
                        // TODO: This is a naive solution to uniformely support the same time of input/output for all
                        // upscalers and post-processor.
                        if (isVPRT) {
                            if (!swapchainState.nonVPRTInputTexture ||
                                view.subImage.imageRect.extent.width !=
                                    swapchainState.nonVPRTInputTexture->getInfo().width ||
                                view.subImage.imageRect.extent.height !=
                                    swapchainState.nonVPRTInputTexture->getInfo().height) {
                                auto createInfo = swapchainImages.appTexture->getInfo();

                                // Single-surface, full (input) screen.
                                createInfo.arraySize = 1;
                                createInfo.width = view.subImage.imageRect.extent.width;
                                createInfo.height = view.subImage.imageRect.extent.height;
                                createInfo.mipCount = 1;

                                // Will be copied to from the app swapchain. Then both upscaler or post-processor will
                                // sample.
                                createInfo.usageFlags =
                                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

                                swapchainState.nonVPRTInputTexture =
                                    m_graphicsDevice->createTexture(createInfo, "Non-VPRT Input TEX2D");
                            }

                            // Patch the top-left corner offset.
                            if (m_upscaleMode == config::ScalingType::NIS ||
                                m_upscaleMode == config::ScalingType::FSR ||
                                m_upscaleMode == config::ScalingType::CAS    
                            ) {
                                correctedProjectionViews[eye].subImage.imageRect.offset.x = (uint32_t)std::ceil(
                                    correctedProjectionViews[eye].subImage.imageRect.offset.x * horizontalScaleFactor);
                                correctedProjectionViews[eye].subImage.imageRect.offset.y = (uint32_t)std::ceil(
                                    correctedProjectionViews[eye].subImage.imageRect.offset.y * verticalScaleFactor);
                            }

                            // Small adjustments to avoid pixel off-texture due to rounding error.
                            if (correctedProjectionViews[eye].subImage.imageRect.offset.x + scaledOutputWidth >
                                swapchainImages.runtimeTexture->getInfo().width) {
                                scaledOutputWidth = swapchainImages.runtimeTexture->getInfo().width -
                                                    correctedProjectionViews[eye].subImage.imageRect.offset.x;
                            }
                            if (correctedProjectionViews[eye].subImage.imageRect.offset.y + scaledOutputHeight >
                                swapchainImages.runtimeTexture->getInfo().height) {
                                scaledOutputHeight = swapchainImages.runtimeTexture->getInfo().height -
                                                     correctedProjectionViews[eye].subImage.imageRect.offset.y;
                            }

                            if (!swapchainState.nonVPRTOutputTexture ||
                                scaledOutputWidth != swapchainState.nonVPRTOutputTexture->getInfo().width ||
                                scaledOutputHeight != swapchainState.nonVPRTOutputTexture->getInfo().height) {
                                auto createInfo = swapchainImages.appTexture->getInfo();

                                // Single-surface, full (output) screen.
                                createInfo.arraySize = 1;
                                createInfo.width = scaledOutputWidth;
                                createInfo.height = scaledOutputHeight;
                                createInfo.mipCount = 1;

                                // Post-processor will draw a full-screen quad. Then will be copied from into the
                                // runtime swapchain.
                                createInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
                                                        XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                                                        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

                                swapchainState.nonVPRTOutputTexture =
                                    m_graphicsDevice->createTexture(createInfo, "Non-VPRT Output TEX2D");
                            }

                            swapchainImages.appTexture->copyTo(view.subImage.imageRect.offset.x,
                                                               view.subImage.imageRect.offset.y,
                                                               view.subImage.imageArrayIndex,
                                                               swapchainState.nonVPRTInputTexture);

                            nextInput = swapchainState.nonVPRTInputTexture;
                            finalOutput = swapchainState.nonVPRTOutputTexture;
                        }

                        // Perform upscaling.
                        if (m_upscaler) {
                            if (!swapchainState.upscaledTexture ||
                                scaledOutputWidth != swapchainState.upscaledTexture->getInfo().width ||
                                scaledOutputHeight != swapchainState.upscaledTexture->getInfo().height) {
                                auto createInfo = swapchainImages.appTexture->getInfo();

                                // Single-surface, full (output) screen.
                                createInfo.arraySize = 1;
                                createInfo.width = scaledOutputWidth;
                                createInfo.height = scaledOutputHeight;

                                // Upscaler will write to as UAV. Then the post-processor will sample.
                                createInfo.usageFlags =
                                    XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

                                if (m_graphicsDevice->isTextureFormatSRGB(createInfo.format)) {
                                    // Good balance between visuals and performance.
                                    createInfo.format =
                                        m_graphicsDevice->getTextureFormat(graphics::TextureFormat::R10G10B10A2_UNORM);
                                }

                                swapchainState.upscaledTexture =
                                    m_graphicsDevice->createTexture(createInfo, "Upscaled TEX2D");
                            }

                            auto timer = swapchainImages.upscalingTimers[eye].get();
                            m_stats.processorGpuTimeUs[0] += timer->query();

                            timer->start();
                            m_upscaler->process(nextInput,
                                                swapchainState.upscaledTexture,
                                                swapchainState.upscalerTextures,
                                                swapchainState.upscalerBlob,
                                                (utilities::Eye)eye);
                            timer->stop();

                            nextInput = swapchainState.upscaledTexture;
                        }

                        // Do post-processing and color conversion.
                        {
                            auto timer = swapchainImages.postProcessingTimers[eye].get();
                            m_stats.processorGpuTimeUs[1] += timer->query();

                            timer->start();
                            m_postProcessor->process(nextInput,
                                                     finalOutput,
                                                     swapchainState.postProcessorTextures,
                                                     swapchainState.postProcessorBlob,
                                                     (utilities::Eye)eye);
                            timer->stop();
                        }

                        // Copy the output back into the VPRT runtime swapchain is needed.
                        if (finalOutput != swapchainImages.runtimeTexture) {
                            finalOutput->copyTo(swapchainImages.runtimeTexture,
                                                correctedProjectionViews[eye].subImage.imageRect.offset.x,
                                                correctedProjectionViews[eye].subImage.imageRect.offset.y,
                                                view.subImage.imageArrayIndex);
                        }

                        // Patch the resolution.
                        correctedProjectionViews[eye].subImage.imageRect.extent.width = scaledOutputWidth;
                        correctedProjectionViews[eye].subImage.imageRect.extent.height = scaledOutputHeight;

                        textureForOverlay[eye] = swapchainImages.runtimeTexture;
                        sliceForOverlay[eye] = view.subImage.imageArrayIndex;
                        depthForOverlay[eye] = depthBuffer;

                        // Patch the eye poses.
                        if (m_configManager->getValue("canting")) {
                            correctedProjectionViews[eye].pose = m_posesForFrame[eye].pose;
                        }

                        // Patch the FOV if it was overriden.
                        const auto fovOverrideMode = m_configManager->peekValue(config::SettingFOVType);
                        if (m_cropActive ||
                            (fovOverrideMode == 0 && m_configManager->peekValue(config::SettingFOV) != 100) ||
                            fovOverrideMode == 1 || m_configManager->peekValue(config::SettingZoom) != 10) {
                            const bool yflip = correctedProjectionViews[eye].fov.angleDown > 0 &&
                                               correctedProjectionViews[eye].fov.angleUp < 0;

                            correctedProjectionViews[eye].fov = m_posesForFrame[eye].fov;

                            // Some apps might modify the FOV, which we don't support in this override case. We
                            // accomodate the most common case here, which is the Y-flip case. For other (uncommon)
                            // cases, we expect the user to not use the override.
                            if (yflip) {
                                std::swap(correctedProjectionViews[eye].fov.angleDown,
                                          correctedProjectionViews[eye].fov.angleUp);
                            }
                        }

                        viewForOverlay[eye].Pose = correctedProjectionViews[eye].pose;
                        viewForOverlay[eye].Fov = correctedProjectionViews[eye].fov;
                        viewForOverlay[eye].NearFar = nearFar;
                        viewportForOverlay[eye] = correctedProjectionViews[eye].subImage.imageRect;
                    }

                    const int icdOverride = m_configManager->getValue(config::SettingICD);
                    if (icdOverride != 1000) {
                        // Restore the original IPD to avoid reprojection being confused.
                        const auto vec =
                            correctedProjectionViews[1].pose.position - correctedProjectionViews[0].pose.position;
                        const auto ipd = Length(vec);
                        const float icd = (ipd * std::max(icdOverride, 1)) / 1000;

                        const auto center = correctedProjectionViews[0].pose.position + (vec * 0.5f);
                        const auto offset = Normalize(vec) * (icd * 0.5f);
                        correctedProjectionViews[0].pose.position = center - offset;
                        correctedProjectionViews[1].pose.position = center + offset;
                    }

                    spaceForOverlay = proj->space;

                    for (uint32_t eye = 0; eye < utilities::ViewCount; eye++) {
                        TraceLoggingWrite(
                            g_traceProvider,
                            "CorrectedView",
                            TLArg("Proj", "Type"),
                            TLArg(eye, "Index"),
                            TLPArg(correctedProjectionViews[eye].subImage.swapchain, "Swapchain"),
                            TLArg(correctedProjectionViews[eye].subImage.imageArrayIndex, "ImageArrayIndex"),
                            TLArg(xr::ToString(correctedProjectionViews[eye].subImage.imageRect).c_str(), "ImageRect"),
                            TLArg(xr::ToString(correctedProjectionViews[eye].pose).c_str(), "Pose"),
                            TLArg(xr::ToString(correctedProjectionViews[eye].fov).c_str(), "Fov"));
                    }

                    correctedProjectionLayer->views = correctedProjectionViews;
                    correctedLayers.push_back(
                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(correctedProjectionLayer));
                } else if (chainFrameEndInfo.layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                    const XrCompositionLayerQuad* quad =
                        reinterpret_cast<const XrCompositionLayerQuad*>(chainFrameEndInfo.layers[i]);

                    TraceLoggingWrite(g_traceProvider,
                                      "xrEndFrame_Layer",
                                      TLArg("Quad", "Type"),
                                      TLArg(quad->layerFlags, "Flags"),
                                      TLPArg(quad->space, "Space"));
                    TraceLoggingWrite(g_traceProvider,
                                      "xrEndFrame_View",
                                      TLArg("Quad", "Type"),
                                      TLPArg(quad->subImage.swapchain, "Swapchain"),
                                      TLArg(quad->subImage.imageArrayIndex, "ImageArrayIndex"),
                                      TLArg(xr::ToString(quad->subImage.imageRect).c_str(), "ImageRect"),
                                      TLArg(xr::ToString(quad->pose).c_str(), "Pose"),
                                      TLArg(quad->size.width, "Width"),
                                      TLArg(quad->size.height, "Height"),
                                      TLArg(xr::ToCString(quad->eyeVisibility), "EyeVisibility"));

                    auto swapchainIt = m_swapchains.find(quad->subImage.swapchain);
                    if (swapchainIt == m_swapchains.end()) {
                        throw std::runtime_error("Swapchain is not registered");
                    }

                    auto& swapchainState = swapchainIt->second;
                    auto& swapchainImages = swapchainState.images[swapchainState.acquiredImageIndex];

                    if (swapchainImages.appTexture != swapchainImages.runtimeTexture) {
                        swapchainImages.appTexture->copyTo(swapchainImages.runtimeTexture);
                    }

                    correctedLayers.push_back(chainFrameEndInfo.layers[i]);
                } else {
                    correctedLayers.push_back(chainFrameEndInfo.layers[i]);
                }
            }

            // We intentionally exclude the overlay from this timer, as it has its own separate timer.
            m_performanceCounters.endFrameCpuTimer->stop();

            // Render our overlays.
            bool needMenuSwapchainDelayedRelease = false;
            {
                const bool drawHands = !isSubBisectOverlaySuppressed() && m_handTracker && m_configManager->peekEnumValue<config::HandTrackingVisibility>(
                                                            config::SettingHandVisibilityAndSkinTone) !=
                                                            config::HandTrackingVisibility::Hidden;
                const bool drawEyeGaze = !isSubBisectOverlaySuppressed() && m_eyeTracker && m_configManager->getValue(config::SettingEyeDebug);

                m_stats.overlayCpuTimeUs += m_performanceCounters.overlayCpuTimer->query();
                m_stats.overlayGpuTimeUs +=
                    m_performanceCounters.overlayGpuTimer[m_performanceCounters.gpuTimerIndex]->query();

                m_performanceCounters.overlayCpuTimer->start();
                m_performanceCounters.overlayGpuTimer[m_performanceCounters.gpuTimerIndex]->start();

                if (textureForOverlay[0]) {
                    const bool useTextureArrays =
                        textureForOverlay[1] == textureForOverlay[0] && sliceForOverlay[0] != sliceForOverlay[1];

                    // Render the hands or eye gaze helper.
                    if (drawHands || drawEyeGaze) {
                        TraceLoggingWrite(
                            g_traceProvider, "StampOverlays", TLArg(drawHands, "Hands"), TLArg(drawEyeGaze, "EyeGaze"));

                        auto isEyeGazeValid = m_eyeTracker && m_eyeTracker->getProjectedGaze(m_eyeGaze);
                        const bool doHandOcclusion = m_configManager->getValue(config::SettingHandOcclusion);

                        for (uint32_t eye = 0; eye < utilities::ViewCount; eye++) {
                            m_graphicsDevice->setRenderTargets(
                                1,
                                &textureForOverlay[eye],
                                useTextureArrays ? reinterpret_cast<int32_t*>(&sliceForOverlay[eye]) : nullptr,
                                &viewportForOverlay[eye],
                                doHandOcclusion ? depthForOverlay[eye] : nullptr,
                                (doHandOcclusion && useTextureArrays) ? eye : -1);

                            m_graphicsDevice->setViewProjection(viewForOverlay[eye]);

                            if (drawHands) {
                                m_handTracker->render(
                                    viewForOverlay[eye].Pose, spaceForOverlay, getTimeNow(), textureForOverlay[eye]);
                            }

                            if (drawEyeGaze) {
                                XrColor4f color = isEyeGazeValid ? XrColor4f{0, 1, 0, 1} : XrColor4f{1, 0, 0, 1};
                                auto pos = utilities::NdcToScreen(m_eyeGaze[eye]);
                                pos.x = viewportForOverlay[eye].offset.x + pos.x * viewportForOverlay[eye].extent.width;
                                pos.y =
                                    viewportForOverlay[eye].offset.y + pos.y * viewportForOverlay[eye].extent.height;
                                m_graphicsDevice->clearColor(pos.y - 20, pos.x - 20, pos.y + 20, pos.x + 20, color);
                            }
                        }

                        m_graphicsDevice->unsetRenderTargets();
                    }
                }

                // Render the menu.
                if (m_menuHandler && !isSubBisectOverlaySuppressed()) {
                    if (!m_configManager->getValue(config::SettingMenuLegacyMode) && !m_configManager->isSafeMode()) {
                        if (m_menuHandler->isVisible() || m_menuLingering) {
                            TraceLoggingWrite(g_traceProvider, "OverlayMenu");

                            // Workaround: there is a bug in the WMR runtime that causes a past quad layer content
                            // to linger on the next projection layer. We make sure to submit a completely blank
                            // quad layer for 3 frames after its disappearance. The number 3 comes from the number
                            // of depth buffers cached inside the precompositor of the WMR runtime.
                            m_menuLingering = m_menuHandler->isVisible() ? 3 : m_menuLingering - 1;

                            uint32_t menuImageIndex;
                            {
                                XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                                CHECK_XRCMD(
                                    OpenXrApi::xrAcquireSwapchainImage(m_menuSwapchain, &acquireInfo, &menuImageIndex));

                                XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                                waitInfo.timeout = XR_INFINITE_DURATION;
                                CHECK_XRCMD(OpenXrApi::xrWaitSwapchainImage(m_menuSwapchain, &waitInfo));
                            }

                            const auto& textureInfo = m_menuSwapchainImages[menuImageIndex]->getInfo();

                            m_graphicsDevice->setRenderTargets(1, &m_menuSwapchainImages[menuImageIndex]);
                            m_graphicsDevice->beginText();
                            m_graphicsDevice->clearColor(
                                0, 0, (float)textureInfo.height, (float)textureInfo.width, XrColor4f{0, 0, 0, 0});
                            m_menuHandler->render(m_menuSwapchainImages[menuImageIndex]);
                            m_graphicsDevice->flushText();

                            m_graphicsDevice->unsetRenderTargets();

                            needMenuSwapchainDelayedRelease = true;

                            // Add the quad layer to the frame.
                            layerQuadForMenu.space = m_viewSpace;
                            StoreXrPose(&layerQuadForMenu.pose,
                                        DirectX::XMMatrixMultiply(
                                            DirectX::XMMatrixTranslation(
                                                0, 0, -m_configManager->getValue(config::SettingMenuDistance) / 100.f),
                                            LoadXrPose(Pose::Identity())));
                            layerQuadForMenu.size.width = layerQuadForMenu.size.height = 1; // 1m x 1m
                            static const XrEyeVisibility visibility[] = {
                                XR_EYE_VISIBILITY_BOTH, XR_EYE_VISIBILITY_LEFT, XR_EYE_VISIBILITY_RIGHT};
                            layerQuadForMenu.eyeVisibility =
                                visibility[std::min(m_configManager->getValue(config::SettingMenuEyeVisibility),
                                                    (int)std::size(visibility))];
                            layerQuadForMenu.subImage.swapchain = m_menuSwapchain;
                            layerQuadForMenu.subImage.imageRect.extent.width = textureInfo.width;
                            layerQuadForMenu.subImage.imageRect.extent.height = textureInfo.height;
                            layerQuadForMenu.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;

                            TraceLoggingWrite(g_traceProvider,
                                              "xrEndFrame_Layer",
                                              TLArg("Quad", "Type"),
                                              TLArg(layerQuadForMenu.layerFlags, "Flags"),
                                              TLPArg(layerQuadForMenu.space, "Space"));
                            TraceLoggingWrite(
                                g_traceProvider,
                                "xrEndFrame_View",
                                TLArg("Quad", "Type"),
                                TLPArg(layerQuadForMenu.subImage.swapchain, "Swapchain"),
                                TLArg(layerQuadForMenu.subImage.imageArrayIndex, "ImageArrayIndex"),
                                TLArg(xr::ToString(layerQuadForMenu.subImage.imageRect).c_str(), "ImageRect"),
                                TLArg(xr::ToString(layerQuadForMenu.pose).c_str(), "Pose"),
                                TLArg(layerQuadForMenu.size.width, "Width"),
                                TLArg(layerQuadForMenu.size.height, "Height"),
                                TLArg(xr::ToCString(layerQuadForMenu.eyeVisibility), "EyeVisibility"));

                            correctedLayers.push_back(
                                reinterpret_cast<XrCompositionLayerBaseHeader*>(&layerQuadForMenu));
                        }
                    } else {
                        // Legacy menu mode, for people having problems.
                        if (textureForOverlay[0]) {
                            TraceLoggingWrite(g_traceProvider, "StampMenu");

                            const bool useTextureArrays = textureForOverlay[1] == textureForOverlay[0] &&
                                                          sliceForOverlay[0] != sliceForOverlay[1];

                            for (uint32_t eye = 0; eye < utilities::ViewCount; eye++) {
                                m_graphicsDevice->setRenderTargets(
                                    1,
                                    &textureForOverlay[eye],
                                    useTextureArrays ? reinterpret_cast<int32_t*>(&sliceForOverlay[eye]) : nullptr,
                                    &viewportForOverlay[eye]);
                                m_graphicsDevice->beginText(true /* mustKeepOldContent */);
                                m_menuHandler->render(textureForOverlay[eye], (utilities::Eye)eye);
                                m_graphicsDevice->flushText();
                            }

                            m_graphicsDevice->unsetRenderTargets();
                        }
                    }
                }

                m_performanceCounters.overlayCpuTimer->stop();
                m_performanceCounters.overlayGpuTimer[m_performanceCounters.gpuTimerIndex]->stop();
            }

            // Whether the menu is available or not, we can still use that top-most texture for screenshot.
            // TODO: The screenshot does not work with multi-layer applications.
            const bool requestScreenshot =
                utilities::UpdateKeyState(m_requestScreenShotKeyState, m_keyModifiers, m_keyScreenshot, false) &&
                m_configManager->getValue(config::SettingScreenshotEnabled);

            if (textureForOverlay[0] && requestScreenshot) {
                // TODO: this is capturing frame N-3
                // review the command queues/lists and context flush
                if (m_configManager->getValue(config::SettingScreenshotEye) != 2 /* Right only */) {
                    takeScreenshot(textureForOverlay[0], "L", viewportForOverlay[0]);
                }
                if (textureForOverlay[1] &&
                    m_configManager->getValue(config::SettingScreenshotEye) != 1 /* Left only */) {
                    takeScreenshot(textureForOverlay[1], "R", viewportForOverlay[1]);
                }

                if (m_variableRateShader && m_configManager->getValue("vrs_capture")) {
                    m_variableRateShader->startCapture();
                }
            }

            m_graphicsDevice->restoreContext();
            m_graphicsDevice->flushContext(false, true);

            // Release the swapchain images now, as we are really done this time.
            for (auto& swapchain : m_swapchains) {
                if (swapchain.second.delayedRelease) {
                    TraceLoggingWrite(g_traceProvider, "DelayedSwapchainRelease", TLPArg(swapchain.first, "Swapchain"));

                    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    swapchain.second.delayedRelease = false;
                    CHECK_XRCMD(OpenXrApi::xrReleaseSwapchainImage(swapchain.first, &releaseInfo));
                }
            }
            if (needMenuSwapchainDelayedRelease) {
                TraceLoggingWrite(g_traceProvider, "MenuSwapchainRelease", TLPArg(m_menuSwapchain, "Swapchain"));

                XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                CHECK_XRCMD(OpenXrApi::xrReleaseSwapchainImage(m_menuSwapchain, &releaseInfo));
            }

            chainFrameEndInfo.layers = correctedLayers.data();
            chainFrameEndInfo.layerCount = (uint32_t)correctedLayers.size();

#if 0
            // When using prediction dampening, we want to restore the display time in order to avoid confusing motion
            // reprojection.
            const bool isMotionReprojectionOn =
                m_supportMotionReprojectionLock &&
                m_configManager->getEnumValue<config::MotionReprojection>(config::SettingMotionReprojection) ==
                    config::MotionReprojection::On;
            if (m_hasPerformanceCounterKHR && m_configManager->getValue(config::SettingPredictionDampen) != 100 &&
                !isMotionReprojectionOn) {
                chainFrameEndInfo.displayTime = m_savedFrameTime2;
            }
#endif

            {
                if (m_asyncWaitPromise.valid()) {
                    TraceLocalActivity(local);

                    // This is the latest point we must have fully waited a frame before proceeding.
                    //
                    // Note: we should not wait infinitely here, however certain patterns of engine calls may cause us
                    // to attempt a "double xrWaitFrame" when turning on Turbo. Use a timeout to detect that, and
                    // refrain from enqueing a second wait further down. This isn't a pretty solution, but it is simple
                    // and it seems to work effectively (minus the 1s freeze observed in-game).
                    TraceLoggingWriteStart(local, "AsyncWaitNow");
                    const auto ready = m_asyncWaitPromise.wait_for(1s) == std::future_status::ready;
                    TraceLoggingWriteStop(local, "AsyncWaitNow", TLArg(ready, "Ready"));
                    if (ready) {
                        m_asyncWaitPromise = {};
                    }

                    CHECK_XRCMD(OpenXrApi::xrBeginFrame(m_vrSession, nullptr));
                }

                const auto result = OpenXrApi::xrEndFrame(session, &chainFrameEndInfo);

                m_graphicsDevice->unblockCallbacks();

                if (m_configManager->getValue(config::SettingTurboMode) && !m_asyncWaitPromise.valid()) {
                    m_asyncWaitPolled = false;
                    m_asyncWaitCompleted = false;

                    // In Turbo mode, we kick off a wait thread immediately.
                    TraceLoggingWrite(g_traceProvider, "AsyncWaitStart");
                    m_asyncWaitPromise = std::async(std::launch::async, [&] {
                        TraceLocalActivity(local);

                        XrFrameState frameState{XR_TYPE_FRAME_STATE};
                        TraceLoggingWriteStart(local, "AsyncWaitFrame");
                        CHECK_XRCMD(OpenXrApi::xrWaitFrame(m_vrSession, nullptr, &frameState));
                        TraceLoggingWriteStop(local,
                                              "AsyncWaitFrame",
                                              TLArg(frameState.predictedDisplayTime, "PredictedDisplayTime"),
                                              TLArg(frameState.predictedDisplayPeriod, "PredictedDisplayPeriod"));
                        {
                            std::unique_lock lock(m_asyncWaitLock);
                            m_lastPredictedDisplayTime = frameState.predictedDisplayTime;
                            m_lastPredictedDisplayPeriod = frameState.predictedDisplayPeriod;

                            m_asyncWaitCompleted = true;
                        }
                    });
                }

                return result;
            }
        }

      private:
        void invalidateProxyScope(const std::string& reason) {
            m_proxyScopeStrict = false;
            if (m_proxyScopeReason.empty()) m_proxyScopeReason = reason;
        }

        void beginProxyScopeSession(ID3D12CommandQueue* queue) {
            m_proxyScopeDecisions.clear();
            m_proxyScopeReleaseLogs.clear();
            m_proxyScopeQueue = queue;
            m_proxyScopeStrict = m_proxyUsageReferenceValid;
            m_proxyScopeReason.clear();
            if (!m_proxyScopeStrict) invalidateProxyScope("reference_missing_incomplete_or_context_mismatch");
            Log("[PROXY-SCOPE] app=%s mode=%u reference_path=%s reference_valid=%u reference_complete=%u "
                "classification=cross_run_observed_role fallback=control_proxy reference_read_only=1 "
                "strict_scope_comparison=%u reason=%s queue=%p\n", m_applicationName.c_str(), m_proxyScopeMode,
                m_proxyUsagePath.string().c_str(), m_proxyUsageReferenceValid, m_proxyUsageReference.complete,
                m_proxyScopeStrict, m_proxyScopeReason.empty() ? "none" : m_proxyScopeReason.c_str(), queue);
        }

        proxy_scope::Decision selectProxyScope(const XrSwapchainCreateInfo& original) const {
            const auto found = m_proxyUsageReference.swapchains.find(m_proxyUsageCreationId + 1);
            return proxy_scope::Select(original, m_proxyScopeMode,
                found == m_proxyUsageReference.swapchains.end() ? nullptr : &found->second,
                m_proxyUsageReferenceValid);
        }

        void recordProxyScopeCreation(XrSwapchain swapchain, const XrSwapchainCreateInfo& original,
                                       const proxy_usage::Snapshot& snapshot, ProxyUsageState& identity) {
            auto& decision = m_proxyScopeDecisions.at(identity.id);
            const auto found = m_proxyUsageReference.swapchains.find(identity.id);
            size_t runtimeFields = 0;
            if (decision.strict && found != m_proxyUsageReference.swapchains.end()) {
                for (const auto& field : found->second) {
                    runtimeFields += field.first.find(".runtime.") != std::string::npos;
                }
                const auto mismatch = proxy_scope::CompareResources(found->second, snapshot, decision.hasProxy);
                if (!mismatch.empty()) {
                    decision.strict = false;
                    decision.reason = "resource_" + mismatch;
                }
            }
            if (!identity.strict) {
                decision.strict = false;
                if (decision.reason.empty()) decision.reason = identity.reason;
            }
            if (!decision.strict) invalidateProxyScope(decision.reason);
            Log("[PROXY-SCOPE] event=create mode=%u creation_id=%llu swapchain=%p reference_role=%u "
                "current_depth=%u selected_path=%s width=%u height=%u arraySize=%u faceCount=%u mipCount=%u "
                "sampleCount=%u requested_format=%lld original_usage=0x%llx createFlags=0x%llx "
                "signature_match=%u strict_scope_comparison=%u reason=%s image_count=%zu "
                "runtime_fields_compared=%zu runtime_descriptor_heap_reference=%s\n", m_proxyScopeMode,
                static_cast<unsigned long long>(identity.id), swapchain, decision.referenceRole,
                (original.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0,
                decision.hasProxy ? "proxy" : "direct", original.width, original.height, original.arraySize,
                original.faceCount, original.mipCount, original.sampleCount, static_cast<long long>(original.format),
                static_cast<unsigned long long>(original.usageFlags), static_cast<unsigned long long>(original.createFlags),
                decision.signatureMatch, m_proxyScopeStrict, decision.reason.empty() ? "none" : decision.reason.c_str(),
                m_swapchains.at(swapchain).images.size(), runtimeFields,
                runtimeFields ? "available_fields_only" : "unavailable_in_reference");
        }

        void validateProxyScopeRole(uint64_t id, uint32_t observed, bool emit) {
            auto& decision = m_proxyScopeDecisions.at(id);
            const bool match = decision.referenceRole == observed;
            if (!match) {
                decision.strict = false;
                decision.reason = "role_mismatch";
                invalidateProxyScope("role_mismatch");
            }
            if (emit) {
                Log("[PROXY-SCOPE] event=role creation_id=%llu frame=%llu reference_role=%u "
                    "observed_current_role=%u projection_observed=%u other_observed=%u role_match=%u "
                    "selected_path=%s strict_scope_comparison=%u reason=%s observation_only=1\n",
                    static_cast<unsigned long long>(id), static_cast<unsigned long long>(m_proxyUsageFrame),
                    decision.referenceRole, observed, (observed & 1) != 0, (observed & 2) != 0, match,
                    decision.hasProxy ? "proxy" : "direct", m_proxyScopeStrict, match ? "none" : "role_mismatch");
            }
        }

        void logProxyScopeRelease(XrSwapchain swapchain, XrResult result, bool copiedNow) {
            // Called after the existing helper has returned from downstream release.
            const auto id = m_proxyUsageSwapchains.at(swapchain).id;
            auto& decision = m_proxyScopeDecisions.at(id);
            if (XR_FAILED(result)) {
                decision.strict = false;
                decision.reason = "runtime_release_failed";
                invalidateProxyScope(decision.reason);
            }
            if (decision.hasProxy) {
                const auto& submission = m_proxyTimingCopies.at(swapchain).submission;
                if (submission.queue != m_proxyScopeQueue || submission.bindingQueue != m_proxyScopeQueue ||
                    !submission.submissionFence || submission.completedAfterCopyWait < submission.submissionFence) {
                    decision.strict = false;
                    decision.reason = "queue_or_copy_completion_mismatch";
                    invalidateProxyScope(decision.reason);
                }
            }
            if (m_proxyScopeReleaseLogs[id]++ < 12) {
                Log("[PROXY-SCOPE] event=release mode=%u creation_id=%llu selected_path=%s "
                    "copy_performed=%u operation=%s copy_point=release runtime_release_point=release "
                    "queue=%p strict_scope_comparison=%u runtime_release_result=%s\n",
                    m_proxyScopeMode, static_cast<unsigned long long>(id), decision.hasProxy ? "proxy" : "direct",
                    copiedNow, copiedNow ? "CopyResource" : "none", m_proxyScopeQueue, m_proxyScopeStrict,
                    xr::ToCString(result));
            }
        }

        void finishProxyScopeSession() {
            uint32_t projectionProxy = 0, otherProxy = 0, projectionDirect = 0, otherDirect = 0;
            if (!m_proxyUsageReferenceValid || m_proxyUsageReference.swapchains.size() != m_proxyUsageCurrent.swapchains.size()) {
                invalidateProxyScope("creation_sequence_or_reference_mismatch");
            }
            for (const auto& entry : m_proxyUsageCurrent.swapchains) {
                const auto roles = entry.second.find("observedRoles");
                if (roles == entry.second.end()) {
                    invalidateProxyScope("current_roles_missing");
                } else {
                    // Current roles are written exclusively by the observer as 0..3.
                    validateProxyScopeRole(entry.first, static_cast<uint32_t>(std::stoul(roles->second)), false);
                }
                const auto& decision = m_proxyScopeDecisions.at(entry.first);
                if (decision.referenceRole == 1) {
                    decision.hasProxy ? ++projectionProxy : ++projectionDirect;
                } else if (decision.referenceRole == 2) {
                    decision.hasProxy ? ++otherProxy : ++otherDirect;
                }
            }
            Log("[PROXY-SCOPE] event=final_summary mode=%u creations=%zu frames=%llu "
                "projection_proxy_count=%u other_proxy_count=%u projection_direct_count=%u other_direct_count=%u "
                "strict_scope_comparison=%u reference_complete=%u reason=%s reference_read_only=1\n",
                m_proxyScopeMode, m_proxyScopeDecisions.size(), static_cast<unsigned long long>(m_proxyUsageFrame),
                projectionProxy, otherProxy, projectionDirect, otherDirect, m_proxyScopeStrict,
                m_proxyUsageReference.complete, m_proxyScopeReason.empty() ? "none" : m_proxyScopeReason.c_str());
        }

        template <typename T>
        static void usageField(proxy_usage::Snapshot& snapshot, const std::string& key, T value) {
            snapshot[key] = std::to_string(value);
        }

        void writeProxyUsageReference() {
            if (m_proxyScopeEnabled) return; // The trusted U0 profile is read-only in ALL scope modes.
            // Creation/teardown only: never called in the completion-to-release interval.
            auto temporary = m_proxyUsagePath;
            temporary += fmt::format(".{}.tmp", GetCurrentProcessId());
            bool written = false;
            try {
                std::ofstream output(temporary, std::ios::trunc);
                written = proxy_usage::WriteReference(output, m_proxyUsageCurrent);
                output.close();
                written = written && bool(output) && MoveFileExW(temporary.c_str(), m_proxyUsagePath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
            } catch (const std::exception&) {
                written = false;
            }
            if (!written) {
                m_proxyUsageRunStrict = false;
                Log("[PROXY-USAGE] reference_write_failed path=%s strict_comparison=0\n",
                    m_proxyUsagePath.string().c_str());
            }
        }

        void beginProxyUsageSession(XrSession session, ID3D12Device* device) {
            m_proxyUsageSwapchains.clear();
            m_proxyUsageCurrent = {};
            m_proxyUsageReference = {};
            m_proxyUsageCreationId = 0;
            m_proxyUsageFrame = 0;
            m_proxyUsageRunStrict = true;
            const auto luid = device->GetAdapterLuid();
            m_proxyUsageCurrent.identity = fmt::format("usage-v1/base-70af555/{}|{}|{}|adapter={}:{}",
                m_proxyUsageAppIdentity, m_runtimeName, m_systemName, luid.HighPart, luid.LowPart);
            m_proxyUsagePath = localAppData / "configs" /
                fmt::format("proxy_usage_{}_U0.txt", m_applicationName);
            std::error_code error;
            std::filesystem::create_directories(m_proxyUsagePath.parent_path(), error);
            m_proxyUsageReferenceValid = false;
            if (m_proxyUsageMode == 1 || m_proxyScopeEnabled) {
                const auto size = std::filesystem::file_size(m_proxyUsagePath, error);
                if (!error && size <= 4 * 1024 * 1024) {
                    std::ifstream input(m_proxyUsagePath);
                    m_proxyUsageReferenceValid = proxy_usage::ReadReference(input, m_proxyUsageReference) &&
                        m_proxyUsageReference.complete &&
                        m_proxyUsageReference.identity == m_proxyUsageCurrent.identity;
                }
                m_proxyUsageRunStrict = m_proxyUsageReferenceValid;
            } else {
                // Invalidate the previous profile now, so an interrupted U0 cannot leave a stale complete reference.
                writeProxyUsageReference();
            }
            Log("[PROXY-USAGE] session=%p mode=%u reference=%s reference_valid=%u "
                "strict_comparison=%u comparison=%s reason=%s\n", session, m_proxyUsageMode,
                m_proxyUsagePath.string().c_str(), m_proxyUsageReferenceValid, m_proxyUsageRunStrict,
                m_proxyUsageMode ? "cross_run_U0" : "reference_self",
                m_proxyUsageRunStrict ? "none" : "U0_reference_missing_incomplete_or_context_mismatch");
        }

        bool captureProxyUsageResource(ID3D12Resource* resource, const char* kind,
                                       const ProxyUsageState& identity, XrSwapchain swapchain, uint32_t index,
                                       proxy_usage::Snapshot& snapshot, bool compareResource) {
            const auto desc = resource->GetDesc();
            Log("[PROXY-RESOURCE] session=%p swapchain=%p generation=%llu creation_id=%llu "
                "image_index=%u kind=%s resource=%p Dimension=%u Alignment=%llu Width=%llu Height=%u "
                "DepthOrArraySize=%u MipLevels=%u Format=%u SampleCount=%u SampleQuality=%u Layout=%u Flags=0x%x\n",
                identity.session, swapchain, static_cast<unsigned long long>(identity.generation),
                static_cast<unsigned long long>(identity.id), index, kind, resource, desc.Dimension,
                static_cast<unsigned long long>(desc.Alignment), static_cast<unsigned long long>(desc.Width),
                desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.Format, desc.SampleDesc.Count,
                desc.SampleDesc.Quality, desc.Layout, desc.Flags);
            const auto prefix = fmt::format("image{}.{}.", index, kind);
            if (compareResource || m_proxyScopeEnabled) {
                usageField(snapshot, prefix + "Dimension", static_cast<uint32_t>(desc.Dimension));
                usageField(snapshot, prefix + "Alignment", desc.Alignment);
                usageField(snapshot, prefix + "Width", desc.Width);
                usageField(snapshot, prefix + "Height", desc.Height);
                usageField(snapshot, prefix + "DepthOrArraySize", desc.DepthOrArraySize);
                usageField(snapshot, prefix + "MipLevels", desc.MipLevels);
                usageField(snapshot, prefix + "Format", static_cast<uint32_t>(desc.Format));
                usageField(snapshot, prefix + "SampleCount", desc.SampleDesc.Count);
                usageField(snapshot, prefix + "SampleQuality", desc.SampleDesc.Quality);
                usageField(snapshot, prefix + "Layout", static_cast<uint32_t>(desc.Layout));
                usageField(snapshot, prefix + "Flags", static_cast<uint32_t>(desc.Flags));
            }
            D3D12_HEAP_PROPERTIES heap{};
            D3D12_HEAP_FLAGS flags{};
            const auto heapResult = resource->GetHeapProperties(&heap, &flags);
            if (SUCCEEDED(heapResult)) {
                Log("[PROXY-RESOURCE] creation_id=%llu image_index=%u kind=%s GetHeapProperties=0x%08x "
                    "Type=%u CPUPageProperty=%u MemoryPoolPreference=%u CreationNodeMask=%u "
                    "VisibleNodeMask=%u HeapFlags=0x%x\n", static_cast<unsigned long long>(identity.id), index,
                    kind, static_cast<unsigned>(heapResult), heap.Type, heap.CPUPageProperty,
                    heap.MemoryPoolPreference, heap.CreationNodeMask, heap.VisibleNodeMask, flags);
                if (compareResource || m_proxyScopeEnabled) {
                    usageField(snapshot, prefix + "HeapType", static_cast<uint32_t>(heap.Type));
                    usageField(snapshot, prefix + "CPUPageProperty", static_cast<uint32_t>(heap.CPUPageProperty));
                    usageField(snapshot, prefix + "MemoryPoolPreference", static_cast<uint32_t>(heap.MemoryPoolPreference));
                    usageField(snapshot, prefix + "CreationNodeMask", heap.CreationNodeMask);
                    usageField(snapshot, prefix + "VisibleNodeMask", heap.VisibleNodeMask);
                    usageField(snapshot, prefix + "HeapFlags", static_cast<uint32_t>(flags));
                }
            } else {
                Log("[PROXY-RESOURCE] creation_id=%llu image_index=%u kind=%s GetHeapProperties=0x%08x "
                    "heap=unavailable\n", static_cast<unsigned long long>(identity.id), index, kind,
                    static_cast<unsigned>(heapResult));
            }
            ComPtr<ID3D12Device> device;
            auto planeResult = resource->GetDevice(IID_PPV_ARGS(set(device)));
            D3D12_FEATURE_DATA_FORMAT_INFO formatInfo{desc.Format, 0};
            if (SUCCEEDED(planeResult)) {
                planeResult = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo));
            }
            if (SUCCEEDED(planeResult)) {
                Log("[PROXY-RESOURCE] creation_id=%llu image_index=%u kind=%s plane_query=0x%08x "
                    "real_format=%u plane_count=%u mip_count=%u array_size=%u samples=%u quality=%u\n",
                    static_cast<unsigned long long>(identity.id), index, kind, static_cast<unsigned>(planeResult),
                    desc.Format, formatInfo.PlaneCount, desc.MipLevels, desc.DepthOrArraySize,
                    desc.SampleDesc.Count, desc.SampleDesc.Quality);
                if (compareResource || m_proxyScopeEnabled) {
                    usageField(snapshot, prefix + "PlaneCount", formatInfo.PlaneCount);
                }
            } else {
                Log("[PROXY-RESOURCE] creation_id=%llu image_index=%u kind=%s plane_query=0x%08x "
                    "plane_count=unavailable real_format=%u\n", static_cast<unsigned long long>(identity.id),
                    index, kind, static_cast<unsigned>(planeResult), desc.Format);
            }
            // Runtime heap changes are allowed consequences of downstream usage; proxy observations must be complete.
            return !compareResource || (SUCCEEDED(heapResult) && SUCCEEDED(planeResult));
        }

        void recordProxyUsageCreation(XrSession session, XrSwapchain swapchain,
                                       const XrSwapchainCreateInfo& original,
                                       const XrSwapchainCreateInfo& downstream, D3D12_RESOURCE_STATES initialState) {
            auto& identity = m_proxyUsageSwapchains[swapchain];
            identity.session = session;
            identity.id = ++m_proxyUsageCreationId;
            identity.generation = ++m_proxyUsageGenerations[swapchain];
            identity.hasProxy = m_subProxySwapchains.at(swapchain);
            identity.strict = true;
            proxy_usage::Snapshot snapshot;
            usageField(snapshot, "width", original.width);
            usageField(snapshot, "height", original.height);
            usageField(snapshot, "arraySize", original.arraySize);
            usageField(snapshot, "faceCount", original.faceCount);
            usageField(snapshot, "mipCount", original.mipCount);
            usageField(snapshot, "sampleCount", original.sampleCount);
            usageField(snapshot, "requestedFormat", original.format);
            usageField(snapshot, "createFlags", original.createFlags);
            usageField(snapshot, "originalUsage", original.usageFlags);
            usageField(snapshot, "hasProxy", identity.hasProxy);
            usageField(snapshot, "initialLogicalState", static_cast<uint32_t>(initialState));
            snapshot["copyPolicy"] = "CopyResource/release/allocatorSafe/currentCompletion/immediateRelease/sameQueue";
            snapshot["proxyCreation"] = "original+runtimeFormat/committed/DEFAULT/SHARED/node1/clearNull/dataNull";
            const auto& state = m_swapchains.at(swapchain);
            usageField(snapshot, "imageCount", state.images.size());
            if (state.images.empty()) {
                identity.strict = false;
                identity.reason = "no_swapchain_images";
            }
            if (original.next) {
                identity.strict = false;
                identity.reason = "uncompared_createInfo_next_chain";
            }
            for (uint32_t i = 0; i < state.images.size(); ++i) {
                const auto& image = state.images[i];
                auto* runtime = image.runtimeTexture->getAs<graphics::D3D12>();
                auto* proxy = image.appTexture->getAs<graphics::D3D12>();
                const auto runtimeDesc = runtime->GetDesc();
                const auto proxyDesc = proxy->GetDesc();
                usageField(snapshot, fmt::format("image{}.runtimeFormat", i), static_cast<uint32_t>(runtimeDesc.Format));
                Log("[PROXY-RESOURCE] session=%p swapchain=%p generation=%llu creation_id=%llu image_index=%u "
                    "runtime_resource=%p proxy_resource=%p has_proxy=%u role=unknown "
                    "initial_state=0x%x states=wrapper_tracked_logical\n", session, swapchain,
                    static_cast<unsigned long long>(identity.generation), static_cast<unsigned long long>(identity.id),
                    i, runtime, proxy, identity.hasProxy, initialState);
                captureProxyUsageResource(runtime, "runtime", identity, swapchain, i, snapshot, false);
                if (!captureProxyUsageResource(proxy, "proxy", identity, swapchain, i, snapshot, true) &&
                    (!m_proxyScopeEnabled || identity.hasProxy)) {
                    identity.strict = false;
                    identity.reason = "proxy_heap_or_plane_query_unavailable";
                }
                if (identity.hasProxy && (runtime == proxy || runtimeDesc.Dimension != proxyDesc.Dimension ||
                    runtimeDesc.Width != proxyDesc.Width || runtimeDesc.Height != proxyDesc.Height ||
                    runtimeDesc.DepthOrArraySize != proxyDesc.DepthOrArraySize ||
                    runtimeDesc.MipLevels != proxyDesc.MipLevels || runtimeDesc.Format != proxyDesc.Format ||
                    runtimeDesc.SampleDesc.Count != proxyDesc.SampleDesc.Count ||
                    runtimeDesc.SampleDesc.Quality != proxyDesc.SampleDesc.Quality)) {
                    identity.strict = false;
                    identity.reason = "native_CopyResource_precondition_mismatch";
                }
            }
            if (m_proxyScopeEnabled) {
                recordProxyScopeCreation(swapchain, original, snapshot, identity);
            } else if (m_proxyUsageMode == 1) {
                const auto found = m_proxyUsageReference.swapchains.find(identity.id);
                if (!m_proxyUsageReferenceValid || found == m_proxyUsageReference.swapchains.end()) {
                    identity.strict = false;
                    identity.reason = "U0_reference_missing_incomplete_context_or_creation_id_mismatch";
                } else {
                    auto baseline = found->second;
                    baseline.erase("observedRoles");
                    const auto mismatch = proxy_usage::Compare(baseline, snapshot);
                    if (!mismatch.empty()) {
                        identity.strict = false;
                        identity.reason = mismatch;
                        const auto before = baseline.find(mismatch), after = snapshot.find(mismatch);
                        Log("[PROXY-USAGE] creation_id=%llu invariant=%s U0=%s current=%s strict_comparison=0\n",
                            static_cast<unsigned long long>(identity.id), mismatch.c_str(),
                            before == baseline.end() ? "missing" : before->second.c_str(),
                            after == snapshot.end() ? "missing" : after->second.c_str());
                    }
                }
            }
            m_proxyUsageRunStrict = m_proxyUsageRunStrict && identity.strict;
            snapshot["observedRoles"] = "0";
            m_proxyUsageCurrent.swapchains.emplace(identity.id, std::move(snapshot));
            Log("[PROXY-USAGE] mode=%u session=%p swapchain=%p generation=%llu creation_id=%llu "
                "width=%u height=%u arraySize=%u faceCount=%u mipCount=%u sampleCount=%u requested_format=%lld "
                "createFlags=0x%llx original_usage=0x%llx downstream_usage=0x%llx "
                "transfer_dst_added=%u transfer_dst_already_present=%u downstream_transfer_dst=%u "
                "images=%zu has_proxy=%u role=unknown strict_comparison=%u reason=%s "
                "comparison=%s final_summary_required=1\n", m_proxyUsageMode, session, swapchain,
                static_cast<unsigned long long>(identity.generation), static_cast<unsigned long long>(identity.id),
                original.width, original.height, original.arraySize, original.faceCount, original.mipCount,
                original.sampleCount, static_cast<long long>(original.format),
                static_cast<unsigned long long>(original.createFlags), static_cast<unsigned long long>(original.usageFlags),
                static_cast<unsigned long long>(downstream.usageFlags), downstream.usageFlags != original.usageFlags,
                (original.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0,
                (downstream.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0, state.images.size(), identity.hasProxy,
                identity.strict, identity.reason.empty() ? "none" : identity.reason.c_str(),
                m_proxyUsageMode ? "cross_run_U0" : "reference_self");
            if (m_proxyUsageMode == 0) {
                writeProxyUsageReference();
            }
        }

        void logProxyUsageAcquire(XrSwapchain swapchain, uint32_t index) {
            auto found = m_proxyUsageSwapchains.find(swapchain);
            if (found == m_proxyUsageSwapchains.end()) return;
            auto& identity = found->second;
            ++identity.acquires;
            if (identity.lifecycleLogs++ < 24) {
                const auto& image = m_swapchains.at(swapchain).images.at(index);
                Log("[PROXY-USAGE] mode=%u event=acquire creation_id=%llu swapchain=%p frame=%llu "
                    "runtime_index=%u proxy_index=%u P=%p R=%p has_proxy=%u\n", m_proxyUsageMode,
                    static_cast<unsigned long long>(identity.id), swapchain,
                    static_cast<unsigned long long>(m_proxyUsageFrame), index, index,
                    image.appTexture->getNativePtr(), image.runtimeTexture->getNativePtr(), identity.hasProxy);
            }
        }

        void logProxyUsageRelease(XrSwapchain swapchain, XrResult result) {
            auto& identity = m_proxyUsageSwapchains.at(swapchain);
            ++identity.releases;
            if (XR_FAILED(result)) {
                identity.strict = false;
                identity.reason = "downstream_release_failed";
                m_proxyUsageRunStrict = false;
            }
            const auto& state = m_swapchains.at(swapchain);
            const auto& image = state.images.at(state.acquiredImageIndex);
            if (identity.hasProxy) {
                const auto& copy = m_proxyTimingCopies.at(swapchain);
                const auto& submission = copy.submission;
                if (submission.queue != submission.bindingQueue || !submission.submissionFence ||
                    submission.completedAfterCopyWait < submission.submissionFence) {
                    identity.strict = false;
                    identity.reason = "queue_or_copy_completion_invariant";
                    m_proxyUsageRunStrict = false;
                }
                if (identity.lifecycleLogs < 24) {
                    Log("[PROXY-USAGE] mode=%u event=copy_report_after_runtime_release creation_id=%llu "
                        "swapchain=%p frame=%llu runtime_index=%u proxy_index=%u P=%p R=%p operation=CopyResource "
                        "copy_point=release runtime_release_point=release queue=%p binding_queue=%p queues_match=%u "
                        "allocator_safe=1 wait_current_copy=1 slot=%u fence=%llu completed=%llu "
                        "completion_verified=%u barriers=%u source_before=0x%x source_after=0x%x "
                        "destination_before=0x%x destination_after=0x%x states=wrapper_tracked_logical "
                        "copy_begin_tick=%llu completion_tick=%llu strict_comparison=%u result=%s\n",
                        m_proxyUsageMode, static_cast<unsigned long long>(identity.id), swapchain,
                        static_cast<unsigned long long>(m_proxyUsageFrame), state.acquiredImageIndex,
                        state.acquiredImageIndex, image.appTexture->getNativePtr(), image.runtimeTexture->getNativePtr(),
                        submission.queue, submission.bindingQueue, submission.queue == submission.bindingQueue,
                        submission.submittedSlot.slot, static_cast<unsigned long long>(submission.submissionFence),
                        static_cast<unsigned long long>(submission.completedAfterCopyWait),
                        submission.completedAfterCopyWait >= submission.submissionFence,
                        2u * ((copy.sourceBefore != D3D12_RESOURCE_STATE_COPY_SOURCE) +
                              (copy.destinationBefore != D3D12_RESOURCE_STATE_COPY_DEST)),
                        copy.sourceBefore, image.appTexture->getTrackedStateForDiagnostics(), copy.destinationBefore,
                        image.runtimeTexture->getTrackedStateForDiagnostics(),
                        static_cast<unsigned long long>(copy.copyBeginTick),
                        static_cast<unsigned long long>(copy.completionTick), identity.strict, xr::ToCString(result));
                }
            }
            if (identity.lifecycleLogs++ < 24) {
                Log("[PROXY-USAGE] mode=%u event=downstream_release_return creation_id=%llu swapchain=%p "
                    "frame=%llu runtime_index=%u proxy_index=%u has_proxy=%u runtime_release_point=release "
                    "strict_comparison=%u result=%s\n", m_proxyUsageMode,
                    static_cast<unsigned long long>(identity.id), swapchain,
                    static_cast<unsigned long long>(m_proxyUsageFrame), state.acquiredImageIndex,
                    state.acquiredImageIndex, identity.hasProxy, identity.strict, xr::ToCString(result));
            }
        }

        static const char* proxyUsageRole(uint32_t roles) {
            return roles == 3 ? "both" : roles == 1 ? "projection" : roles == 2 ? "other" : "unknown";
        }

        void observeProxyUsageRole(XrSwapchain swapchain, uint32_t role, uint32_t layer, uint32_t view,
                                    uint32_t slice, const XrRect2Di* rect) {
            auto found = m_proxyUsageSwapchains.find(swapchain);
            if (found == m_proxyUsageSwapchains.end()) return;
            auto& identity = found->second;
            const auto previous = identity.roles;
            identity.roles |= role;
            const bool first = !(previous & role);
            if (first) {
                (role == 1 ? identity.firstProjection : identity.firstOther) = m_proxyUsageFrame;
            }
            m_proxyUsageCurrent.swapchains.at(identity.id)["observedRoles"] = std::to_string(identity.roles);
            const auto rectText = rect ? xr::ToString(*rect) : "not_applicable";
            const auto binding = fmt::format("{}:{}:{}:{}:{}", role, layer, view, slice, rectText);
            const bool newBinding = identity.roleBindings.size() < 16 && identity.roleBindings.insert(binding).second;
            if (m_proxyScopeEnabled) validateProxyScopeRole(identity.id, identity.roles, first || newBinding);
            if (first || newBinding) {
                Log("[PROXY-ROLE] creation_id=%llu swapchain=%p generation=%llu frame=%llu "
                    "use=%s role=%s previous_role=%s first_use=%u role_changed=%u layer=%u view=%u "
                    "imageArrayIndex=%u imageRect=%s observation_only=1\n",
                    static_cast<unsigned long long>(identity.id), swapchain,
                    static_cast<unsigned long long>(identity.generation), static_cast<unsigned long long>(m_proxyUsageFrame),
                    role == 1 ? "projection" : "other", proxyUsageRole(identity.roles), proxyUsageRole(previous),
                    first, identity.roles != previous, layer, view, slice, rectText.c_str());
            }
        }

        void observeProxyUsageRoles(const XrFrameEndInfo* info) {
            // Read application composition only. Never select images, modify structs, copy or release here.
            for (uint32_t i = 0; info->layers && i < info->layerCount; ++i) {
                const auto* layer = info->layers[i];
                if (!layer) continue;
                if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                    for (uint32_t view = 0; projection->views && view < projection->viewCount; ++view) {
                        const auto& image = projection->views[view].subImage;
                        observeProxyUsageRole(image.swapchain, 1, i, view, image.imageArrayIndex, &image.imageRect);
                    }
                } else {
                    const XrSwapchainSubImage* image = nullptr;
                    switch (layer->type) {
                    case XR_TYPE_COMPOSITION_LAYER_QUAD:
                        image = &reinterpret_cast<const XrCompositionLayerQuad*>(layer)->subImage; break;
                    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
                        image = &reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer)->subImage; break;
                    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
                        image = &reinterpret_cast<const XrCompositionLayerEquirectKHR*>(layer)->subImage; break;
                    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
                        image = &reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(layer)->subImage; break;
                    case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: {
                        const auto* cube = reinterpret_cast<const XrCompositionLayerCubeKHR*>(layer);
                        observeProxyUsageRole(cube->swapchain, 2, i, 0, cube->imageArrayIndex, nullptr); break;
                    }
                    default: break; // Unknown extensions are not guessed or classified.
                    }
                    if (image) {
                        observeProxyUsageRole(image->swapchain, 2, i, 0, image->imageArrayIndex, &image->imageRect);
                    }
                }
            }
            ++m_proxyUsageFrame;
        }

        void summarizeProxyUsage(XrSwapchain swapchain) {
            const auto& identity = m_proxyUsageSwapchains.at(swapchain);
            Log("[PROXY-ROLE] event=summary creation_id=%llu swapchain=%p generation=%llu role=%s "
                "projection_observed=%u other_observed=%u first_projection_frame=%llu first_other_frame=%llu "
                "acquires=%llu releases=%llu strict_comparison=%u reason=%s\n",
                static_cast<unsigned long long>(identity.id), swapchain,
                static_cast<unsigned long long>(identity.generation), proxyUsageRole(identity.roles),
                (identity.roles & 1) != 0, (identity.roles & 2) != 0,
                static_cast<unsigned long long>(identity.firstProjection), static_cast<unsigned long long>(identity.firstOther),
                static_cast<unsigned long long>(identity.acquires), static_cast<unsigned long long>(identity.releases),
                identity.strict, identity.reason.empty() ? "none" : identity.reason.c_str());
        }

        void finishProxyUsageSession() {
            if (m_proxyScopeEnabled) finishProxyScopeSession();
            for (const auto& entry : m_proxyUsageSwapchains) summarizeProxyUsage(entry.first);
            std::string reason;
            if (m_proxyUsageMode == 1 && m_proxyUsageReferenceValid) {
                if (m_proxyUsageReference.swapchains.size() != m_proxyUsageCurrent.swapchains.size()) {
                    reason = "total_swapchain_creation_count";
                } else {
                    for (const auto& entry : m_proxyUsageCurrent.swapchains) {
                        auto baseline = m_proxyUsageReference.swapchains.find(entry.first);
                        if (baseline == m_proxyUsageReference.swapchains.end() ||
                            !baseline->second.count("observedRoles") ||
                            baseline->second.at("observedRoles") != entry.second.at("observedRoles")) {
                            reason = fmt::format("creation_id_{}_observed_roles", entry.first);
                            break;
                        }
                    }
                }
                if (!reason.empty()) m_proxyUsageRunStrict = false;
            }
            if (m_proxyUsageMode == 0) {
                m_proxyUsageCurrent.complete = m_proxyUsageRunStrict && !m_proxyUsageCurrent.swapchains.empty();
                writeProxyUsageReference();
            }
            Log("[PROXY-USAGE] event=final_summary mode=%u creations=%zu frames=%llu strict_comparison=%u "
                "reference_complete=%u reason=%s hidden_runtime_properties=uncompared\n", m_proxyUsageMode,
                m_proxyUsageCurrent.swapchains.size(), static_cast<unsigned long long>(m_proxyUsageFrame),
                m_proxyUsageRunStrict, m_proxyUsageMode ? m_proxyUsageReferenceValid : m_proxyUsageCurrent.complete,
                !reason.empty() ? reason.c_str() : m_proxyUsageRunStrict ? "none" : "see_creation_or_reference_errors");
            m_proxyUsageSwapchains.clear();
        }

        bool isVrSystem(XrSystemId systemId) const {
            return systemId == m_vrSystemId;
        }

        bool isVrSession(XrSession session) const {
            return session == m_vrSession;
        }

        bool isGraphicsNeutralApp() const {
            return m_applicationName == "Impact" || m_applicationName == "TheMidnightWalk";
        }

        bool isMetroGraphicsSession(XrSession session) const {
            return m_metroGraphicsSession != XR_NULL_HANDLE && session == m_metroGraphicsSession;
        }

        bool isSubBisectOverlaySuppressed() const {
            return isGraphicsNeutralApp() && m_gfxSubBisectMode == 4;
        }

        void logGraphicsSubBisectMode() const {
            static constexpr const char* copyPoint[] = {"none", "none", "release", "endFrame",
                                                       "processing_chain", "processing_chain"};
            const uint32_t mode = m_gfxSubBisectMode;
            Log("[GFX-SUBBISECT] app=%s submode=%u proxy=%u runtime_image_exposed=%u "
                "delayed_release=%u copy=%u copy_point=%s overlay=%u postprocess=%u "
                "command_lists=1 fences=1 flush=%s interceptor=0 analyzer=0\n",
                m_applicationName.c_str(), mode, mode >= 2, mode < 2, mode == 1 || mode == 3 || mode >= 4,
                mode >= 2, copyPoint[mode], mode == 5, mode >= 4,
                mode >= 4 ? "normal" : "mode2_plus_copy_if_needed");
        }

        XrResult releaseSubDirectImage(XrSwapchain swapchain, const char* point) {
            XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const uint32_t imageIndex = m_metroSwapchainIndices.at(swapchain);
            const XrResult result = OpenXrApi::xrReleaseSwapchainImage(swapchain, &info);
            if (XR_SUCCEEDED(result)) {
                m_subPendingDirectRelease[swapchain] = false;
                m_metroSwapchainIndices[swapchain] = UINT32_MAX;
            }
            if (m_subReleaseLogs.fetch_add(1) < MetroGfxLogSamples) {
                Log("[GFX-SUBBISECT] app=%s submode=1 downstream_release point=%s swapchain=%p "
                    "runtime_index=%u result=%s copy=0\n", m_applicationName.c_str(), point,
                    swapchain, imageIndex, xr::ToCString(result));
            }
            return result;
        }

        graphics::ProxySyncSubmission copySubProxyImage(XrSwapchain swapchain, const char* point) {
            auto& state = m_swapchains.at(swapchain);
            const uint32_t imageIndex = state.acquiredImageIndex;
            const bool hasProxy = m_subProxySwapchains.at(swapchain);
            if (!hasProxy) {
                return {};
            }

            auto& images = state.images.at(imageIndex);
            const auto sourceBefore = images.appTexture->getTrackedStateForDiagnostics();
            const auto destinationBefore = images.runtimeTexture->getTrackedStateForDiagnostics();

            // Allocate diagnostic storage before GPU submission, never in the completion-to-release interval.
            auto& diagnostic = m_proxyTimingCopies[swapchain];
            const auto copyBeginTick = GetTickCount64();
            // CopyResource covers every mip and array slice. The sync diagnostic is pinned to Mode 2:
            // safe allocator reuse plus verified completion of this copy before this helper returns.
            images.appTexture->pushState(D3D12_RESOURCE_STATE_COPY_SOURCE);
            images.runtimeTexture->pushState(D3D12_RESOURCE_STATE_COPY_DEST);
            m_bisectDevice->getContextAs<graphics::D3D12>()->CopyResource(
                images.runtimeTexture->getAs<graphics::D3D12>(),
                images.appTexture->getAs<graphics::D3D12>());
            images.runtimeTexture->popState();
            images.appTexture->popState();
            const auto submission = m_bisectDevice->flushProxySyncCopy();
            m_subProxyCopyCompleted[swapchain] = true;

            diagnostic = {submission, sourceBefore, destinationBefore,
                copyBeginTick, GetTickCount64()};
            return submission;
        }

        void logProxyTimingCopy(XrSwapchain swapchain, const char* point) {
            const auto& state = m_swapchains.at(swapchain);
            const auto imageIndex = state.acquiredImageIndex;
            const auto& images = state.images.at(imageIndex);
            const auto& diagnostic = m_proxyTimingCopies.at(swapchain);
            const auto& submission = diagnostic.submission;
            const auto sourceBefore = diagnostic.sourceBefore;
            const auto destinationBefore = diagnostic.destinationBefore;
            if (m_proxyTimingLogs.fetch_add(1) < 192) {
                const auto& slot = submission.submittedSlot;
                Log("[PROXY-TIMING] app=%s mode=%u event=copy point=%s swapchain=%p kind=color "
                    "proxy_resource=%p runtime_resource=%p proxy_index=%u runtime_index=%u arraySize=%u "
                    "state_source_before=0x%x state_source_after=0x%x "
                    "state_destination_before=0x%x state_destination_after=0x%x barriers=%u "
                    "slot=%u queue=%p binding_queue=%p queues_match=%u current_fence=%llu "
                    "copy_wait=%u copy_wait_ms=%.3f completed_before_wait=%llu completed_after_wait=%llu "
                    "completion_verified=%u frame=%llu copy_begin_tick=%llu completion_tick=%llu "
                    "states=wrapper_tracked fence_wait_point=same_as_copy result=XR_SUCCESS\n",
                    m_applicationName.c_str(), m_proxyTimingMode, point, swapchain,
                    images.appTexture->getNativePtr(), images.runtimeTexture->getNativePtr(), imageIndex, imageIndex,
                    state.requestedArraySize, sourceBefore, images.appTexture->getTrackedStateForDiagnostics(),
                    destinationBefore, images.runtimeTexture->getTrackedStateForDiagnostics(),
                    2u * ((sourceBefore != D3D12_RESOURCE_STATE_COPY_SOURCE) +
                          (destinationBefore != D3D12_RESOURCE_STATE_COPY_DEST)),
                    slot.slot, submission.queue, submission.bindingQueue,
                    submission.queue == submission.bindingQueue,
                    static_cast<unsigned long long>(submission.submissionFence), submission.copyWaited,
                    submission.copyWaitMs, static_cast<unsigned long long>(submission.completedBeforeCopyWait),
                    static_cast<unsigned long long>(submission.completedAfterCopyWait),
                    submission.completedAfterCopyWait >= submission.submissionFence,
                    static_cast<unsigned long long>(m_proxyTimingFrame),
                    static_cast<unsigned long long>(diagnostic.copyBeginTick),
                    static_cast<unsigned long long>(diagnostic.completionTick));
            }
        }

        XrResult releaseSubProxyImage(XrSwapchain swapchain, const char* point,
                                      const XrSwapchainImageReleaseInfo* originalInfo = nullptr) {
            auto& state = m_swapchains.at(swapchain);
            const uint32_t imageIndex = state.acquiredImageIndex;
            const bool hasProxy = m_subProxySwapchains.at(swapchain);
            bool copiedNow = false;
            graphics::ProxySyncSubmission copySubmission;

            if (hasProxy && !m_subProxyCopyCompleted[swapchain]) {
                copySubmission = copySubProxyImage(swapchain, point);
                copiedNow = true;
            }
            const bool copyWasReady = hasProxy && m_subProxyCopyCompleted[swapchain];

            XrSwapchainImageReleaseInfo defaultInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const auto releaseEnterTick = GetTickCount64();
            const XrResult result = OpenXrApi::xrReleaseSwapchainImage(
                swapchain, originalInfo ? originalInfo : &defaultInfo);
            // Mode 0 retains the base Mode 2's no-log interval between copy and runtime release.
            if (copiedNow) {
                logProxyTimingCopy(swapchain, point);
            }
            if (XR_SUCCEEDED(result)) {
                state.delayedRelease = false;
                m_subProxyCopyCompleted[swapchain] = false;
                m_proxyTimingPendingOrder.erase(std::remove(m_proxyTimingPendingOrder.begin(),
                    m_proxyTimingPendingOrder.end(), swapchain), m_proxyTimingPendingOrder.end());
            }

            if (m_proxyTimingEnabled && m_proxyTimingLogs.fetch_add(1) < 192) {
                Log("[PROXY-TIMING] app=%s mode=%u event=downstream_release point=%s "
                    "swapchain=%p kind=%s image_index=%u copied_now=%u copy_was_ready=%u "
                    "frame=%llu enter_tick=%llu proxy_index=%u runtime_index=%u proxy_resource=%p "
                    "runtime_resource=%p state_source=0x%x state_destination=0x%x "
                    "states=wrapper_tracked barriers_current=%u release_order=%s result=%s\n",
                    m_applicationName.c_str(), m_proxyTimingMode, point, swapchain,
                    hasProxy ? "color" : "depth", imageIndex, copiedNow, copyWasReady,
                    static_cast<unsigned long long>(m_proxyTimingFrame),
                    static_cast<unsigned long long>(releaseEnterTick), imageIndex, imageIndex,
                    state.images.at(imageIndex).appTexture->getNativePtr(),
                    state.images.at(imageIndex).runtimeTexture->getNativePtr(),
                    state.images.at(imageIndex).appTexture->getTrackedStateForDiagnostics(),
                    state.images.at(imageIndex).runtimeTexture->getTrackedStateForDiagnostics(), 0u,
                    std::string_view(point) == "release" ? "app_release" :
                    std::string_view(point) == "endFrame" ? "before_xrEndFrame" : "forced_before_reuse",
                    xr::ToCString(result));
            }
            if (m_subReleaseLogs.fetch_add(1) < MetroGfxLogSamples) {
                Log("[GFX-SUBBISECT] app=%s submode=%u downstream_release point=%s swapchain=%p "
                    "runtime_index=%u proxy_index=%u copy=%u arraySize=%u command_list=%u "
                    "fence_wait=%u result=%s\n", m_applicationName.c_str(), m_gfxSubBisectMode,
                    point, swapchain, imageIndex, imageIndex, hasProxy, state.requestedArraySize,
                    hasProxy && copiedNow, hasProxy && copiedNow && copySubmission.copyWaited, xr::ToCString(result));
            }
            return result;
        }

        void invalidateProxyIndex(const char* reason) {
            ++m_proxyIndexFifoErrors;
            m_proxyIndexStrict = false;
            if (m_proxyIndexReason.empty()) m_proxyIndexReason = reason;
            if (m_proxyIndexErrorLogs++ < 16) {
                Log("[PROXY-INDEX] invalid app=%s mode=%u reason=%s fifo_errors=%llu\n",
                    m_applicationName.c_str(), m_proxyIndexMode, reason,
                    static_cast<unsigned long long>(m_proxyIndexFifoErrors));
            }
        }

        void logProxyIndexRelease(XrSwapchain swapchain, uint32_t oldestAcquiredIndex,
                                  uint32_t oldestWaitedIndex, uint32_t selectedCopyIndex,
                                  uint32_t legacyIndex, bool validIndex, XrResult result,
                                  size_t queueDepthBefore, const proxy_index::Tracker& tracker) {
            if (m_proxyIndexReleaseLogs++ >= 48) return;
            Log("[PROXY-INDEX] event=release app=%s mode=%u swapchain=%p "
                "oldest_acquired_index=%u oldest_waited_index=%u last_acquired_index=%u "
                "selected_copy_index=%u legacy_index=%u mismatch=%u valid=%u "
                "downstream_release_result=%s queue_depth_before=%zu queue_depth_after=%zu queue=%s\n",
                m_applicationName.c_str(), m_proxyIndexMode, swapchain, oldestAcquiredIndex,
                oldestWaitedIndex, legacyIndex, selectedCopyIndex, legacyIndex,
                validIndex && legacyIndex != oldestAcquiredIndex, validIndex, xr::ToCString(result),
                queueDepthBefore, tracker.queue.size(), tracker.state().c_str());
        }

        void finishProxyIndexSession() {
            size_t pendingAtSessionEnd = 0;
            for (const auto& tracker : m_proxyIndexTrackers) {
                pendingAtSessionEnd += tracker.second.queue.size();
            }
            const bool strict = m_proxyIndexStrict && m_proxyScopeStrict && m_proxyIndexReleaseCount > 0;
            const char* reason = !m_proxyIndexReason.empty() ? m_proxyIndexReason.c_str() :
                !m_proxyScopeStrict ? (m_proxyScopeReason.empty() ? "scope_comparison_invalid" :
                                       m_proxyScopeReason.c_str()) :
                m_proxyIndexReleaseCount == 0 ? "no_proxy_releases" : "ok";
            Log("[PROXY-INDEX] final_summary app=%s mode=%u acquire_count=%llu "
                "successful_wait_count=%llu timeout_count=%llu release_count=%llu "
                "legacy_index_mismatch_count=%llu fifo_errors=%llu pending_at_session_end=%zu "
                "pending_at_destroy=%llu strict_comparison=%u reason=%s\n",
                m_applicationName.c_str(), m_proxyIndexMode,
                static_cast<unsigned long long>(m_proxyIndexAcquireCount),
                static_cast<unsigned long long>(m_proxyIndexSuccessfulWaitCount),
                static_cast<unsigned long long>(m_proxyIndexTimeoutCount),
                static_cast<unsigned long long>(m_proxyIndexReleaseCount),
                static_cast<unsigned long long>(m_proxyIndexLegacyMismatchCount),
                static_cast<unsigned long long>(m_proxyIndexFifoErrors), pendingAtSessionEnd,
                static_cast<unsigned long long>(m_proxyIndexPendingAtDestroy), strict, reason);
        }

        const std::string getPath(XrPath path) {
            if (path == XR_NULL_PATH) {
                return "";
            }

            char buf[XR_MAX_PATH_LENGTH];
            uint32_t count;
            CHECK_XRCMD(xrPathToString(GetXrInstance(), path, sizeof(buf), &count, buf));
            std::string str;
            str.assign(buf, count - 1);
            return str;
        }

        // Find the current time. Fallback to the frame time if we cannot query the actual time.
        XrTime getTimeNow() {
            XrTime xrTimeNow = m_begunFrameTime;
            if (m_hasPerformanceCounterKHR) {
                LARGE_INTEGER qpcTimeNow;
                QueryPerformanceCounter(&qpcTimeNow);

                CHECK_XRCMD(xrConvertWin32PerformanceCounterToTimeKHR(GetXrInstance(), &qpcTimeNow, &xrTimeNow));
            }

            return xrTimeNow;
        }

        void createMenuSwapchain() {
            uint32_t formatCount = 0;
            CHECK_XRCMD(xrEnumerateSwapchainFormats(m_vrSession, 0, &formatCount, nullptr));
            std::vector<int64_t> formats(formatCount);
            CHECK_XRCMD(xrEnumerateSwapchainFormats(m_vrSession, formatCount, &formatCount, formats.data()));

            XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            swapchainInfo.width = swapchainInfo.height = 2048; // Let's hope the menu doesn't get bigger than that.
            swapchainInfo.arraySize = 1;
            swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            swapchainInfo.format = formats[0];
            swapchainInfo.sampleCount = 1;
            swapchainInfo.faceCount = 1;
            swapchainInfo.mipCount = 1;
            CHECK_XRCMD(OpenXrApi::xrCreateSwapchain(m_vrSession, &swapchainInfo, &m_menuSwapchain));
            TraceLoggingWrite(g_traceProvider, "MenuSwapchain", TLPArg(m_menuSwapchain, "Swapchain"));

            uint32_t imageCount;
            CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(m_menuSwapchain, 0, &imageCount, nullptr));

            SwapchainState swapchainState;
            int64_t overrideFormat = 0;
            if (m_graphicsDevice->getApi() == graphics::Api::D3D11) {
                std::vector<XrSwapchainImageD3D11KHR> d3dImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(
                    m_menuSwapchain,
                    imageCount,
                    &imageCount,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(d3dImages.data())));

                for (uint32_t i = 0; i < imageCount; i++) {
                    m_menuSwapchainImages.push_back(
                        graphics::WrapD3D11Texture(m_graphicsDevice,
                                                   swapchainInfo,
                                                   d3dImages[i].texture,
                                                   fmt::format("Menu swapchain {} TEX2D", i)));
                }
            } else if (m_graphicsDevice->getApi() == graphics::Api::D3D12) {
                std::vector<XrSwapchainImageD3D12KHR> d3dImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(
                    m_menuSwapchain,
                    imageCount,
                    &imageCount,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(d3dImages.data())));

                for (uint32_t i = 0; i < imageCount; i++) {
                    m_menuSwapchainImages.push_back(
                        graphics::WrapD3D12Texture(m_graphicsDevice,
                                                   swapchainInfo,
                                                   d3dImages[i].texture,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   fmt::format("Menu swapchain {} TEX2D", i)));
                }
            } else {
                throw std::runtime_error("Unsupported graphics runtime");
            }
        }

        std::string m_applicationName;
        bool m_isOpenComposite{false};
        bool m_isUnity{false};
        std::string m_runtimeName;
        std::string m_systemName;
        XrSystemId m_vrSystemId{XR_NULL_SYSTEM_ID};
        XrSession m_vrSession{XR_NULL_HANDLE};
        uint32_t m_gfxBisectMode{0};
        uint32_t m_gfxSubBisectMode{0};
        bool m_proxySyncEnabled{false};
        uint32_t m_proxySyncMode{0};
        bool m_proxyTimingEnabled{false};
        uint32_t m_proxyTimingMode{0};
        std::atomic<uint32_t> m_proxyTimingLogs{0};
        bool m_proxyUsageEnabled{false}, m_proxyUsageReferenceValid{false}, m_proxyUsageRunStrict{true};
        bool m_proxyScopeEnabled{false}, m_proxyScopeStrict{false};
        bool m_proxyIndexEnabled{false}, m_proxyIndexStrict{true};
        uint32_t m_proxyIndexMode{0};
        std::string m_proxyIndexReason;
        std::map<XrSwapchain, proxy_index::Tracker> m_proxyIndexTrackers;
        uint64_t m_proxyIndexAcquireCount{0}, m_proxyIndexSuccessfulWaitCount{0}, m_proxyIndexTimeoutCount{0};
        uint64_t m_proxyIndexReleaseCount{0}, m_proxyIndexLegacyMismatchCount{0}, m_proxyIndexFifoErrors{0};
        uint64_t m_proxyIndexPendingAtDestroy{0};
        uint32_t m_proxyIndexAcquireLogs{0}, m_proxyIndexWaitLogs{0}, m_proxyIndexReleaseLogs{0};
        uint32_t m_proxyIndexErrorLogs{0}, m_proxyIndexTeardownLogs{0};
        uint32_t m_proxyScopeMode{0};
        std::string m_proxyScopeReason;
        ID3D12CommandQueue* m_proxyScopeQueue{nullptr};
        std::map<uint64_t, proxy_scope::Decision> m_proxyScopeDecisions;
        std::map<uint64_t, uint32_t> m_proxyScopeReleaseLogs;
        uint32_t m_proxyUsageMode{0};
        uint64_t m_proxyUsageCreationId{0}, m_proxyUsageFrame{0};
        std::string m_proxyUsageAppIdentity;
        std::filesystem::path m_proxyUsagePath;
        proxy_usage::Reference m_proxyUsageReference, m_proxyUsageCurrent;
        std::map<XrSwapchain, ProxyUsageState> m_proxyUsageSwapchains;
        std::map<XrSwapchain, uint64_t> m_proxyUsageGenerations;
        std::shared_ptr<graphics::IDevice> m_bisectDevice;
        XrSession m_metroGraphicsSession{XR_NULL_HANDLE};
        std::map<XrSwapchain, uint32_t> m_metroSwapchainIndices;
        std::map<XrSwapchain, bool> m_subPendingDirectRelease;
        std::map<XrSwapchain, bool> m_subProxySwapchains;
        std::map<XrSwapchain, bool> m_subProxyCopyCompleted;
        struct ProxyTimingCopy {
            graphics::ProxySyncSubmission submission;
            D3D12_RESOURCE_STATES sourceBefore;
            D3D12_RESOURCE_STATES destinationBefore;
            ULONGLONG copyBeginTick;
            ULONGLONG completionTick;
        };
        std::map<XrSwapchain, ProxyTimingCopy> m_proxyTimingCopies;
        std::vector<XrSwapchain> m_proxyTimingPendingOrder;
        uint64_t m_proxyTimingFrame{0};
        std::atomic<uint32_t> m_subReleaseLogs{0};
        std::atomic<uint32_t> m_metroGfxEnumerationLogs{0};
        std::atomic<uint32_t> m_metroGfxImageLogs{0};
        std::atomic<uint32_t> m_metroGfxWaitLogs{0};
        std::atomic<uint32_t> m_metroGfxAcquireLogs{0};
        std::atomic<uint32_t> m_metroGfxReleaseLogs{0};
        std::atomic<uint32_t> m_metroGfxFrameWaitLogs{0};
        std::atomic<uint32_t> m_metroGfxBeginLogs{0};
        std::atomic<uint32_t> m_metroGfxEndLogs{0};
        uint32_t m_displayWidth{0};
        uint32_t m_displayHeight{0};
        uint32_t m_runtimeRecommendedWidth[utilities::ViewCount]{};
        uint32_t m_runtimeRecommendedHeight[utilities::ViewCount]{};
        uint32_t m_cropRecommendedWidth[utilities::ViewCount]{};
        uint32_t m_cropRecommendedHeight[utilities::ViewCount]{};
        XrFovf m_cropOriginalFov[utilities::ViewCount]{};
        std::string m_cropCalibrationIdentity;
        std::filesystem::path m_cropCalibrationPath;
        int m_cropFovPercent{100};
        bool m_cropActive{false};
        bool m_cropExact{false};
        bool m_cropCacheHit{false};
        bool m_cropCalibrationChecked{false};
        bool m_cropInvalidFovLogged{false};
        bool m_cropEnumerationLogged{false};
        bool m_cropFovLogged{false};
        float m_resolutionHeightRatio{1.f};
        uint32_t m_maxDisplayHeight{0};
        bool m_supportHandTracking{false};
        bool m_supportEyeTracking{false};
        bool m_supportMotionReprojectionLock{false};
        bool m_isOmniceptDetected{false};
        bool m_hasPimaxEyeTracker{false};
        bool m_isFrameThrottlingPossible{true};
        bool m_overrideParallelProjection{false};

        std::mutex m_frameLock;
        XrTime m_waitedFrameTime;
        XrTime m_begunFrameTime;
        XrTime m_savedFrameTime1;
        XrTime m_savedFrameTime2;
        bool m_isInFrame{false};
        bool m_sendInterationProfileEvent{false};
        uint32_t m_visibilityMaskEventIndex{utilities::ViewCount};
        XrSpace m_viewSpace{XR_NULL_HANDLE};
        bool m_needCalibrateEyeProjections{true};
        XrVector2f m_projCenters[utilities::ViewCount];
        XrVector2f m_eyeGaze[utilities::ViewCount];
        XrView m_posesForFrame[utilities::ViewCount];
        std::chrono::time_point<std::chrono::steady_clock> m_lastFrameWaitTimestamp{};
        uint32_t m_frameThrottleSleepOffset{0};

        std::mutex m_asyncWaitLock;
        std::future<void> m_asyncWaitPromise;
        XrTime m_lastPredictedDisplayTime{0};
        XrTime m_lastPredictedDisplayPeriod{0};
        bool m_asyncWaitPolled{false};
        bool m_asyncWaitCompleted{false};

        std::shared_ptr<config::IConfigManager> m_configManager;

        std::shared_ptr<graphics::IDevice> m_graphicsDevice;
        std::map<XrSwapchain, SwapchainState> m_swapchains;

        config::ScalingType m_upscaleMode{config::ScalingType::None};
        int m_settingScaling{100};
        int m_settingAnamorphic{-100};
        float m_mipMapBiasForUpscaling{0.f};

        std::shared_ptr<graphics::IFrameAnalyzer> m_frameAnalyzer;
        std::shared_ptr<input::IEyeTracker> m_eyeTracker;
        bool m_isSuggestingToolkitEyeBinding{false};
        std::map<XrAction, XrActionSet> m_actionSetByAction;
        std::vector<XrActionSuggestedBinding> m_appEyeGazeBindings;
        std::optional<XrActionSuggestedBinding> m_toolkitEyeGazeBinding;
        bool m_isActionSetUsed{false};
        bool m_isActionSetAttached{false};
        bool m_isEyeActionSetSynced{false};
        XrResult m_lastSyncError{XR_SUCCESS};
        bool m_artificialActionsLogged{false};
        bool m_needVarjoPollEventWorkaround{false};
        std::shared_ptr<input::IHandTracker> m_handTracker;

        std::shared_ptr<graphics::IImageProcessor> m_upscaler;
        std::shared_ptr<graphics::IImageProcessor> m_postProcessor;
        std::shared_ptr<graphics::IVariableRateShader> m_variableRateShader;

        std::vector<int> m_keyModifiers;
        int m_keyScreenshot;
        XrSwapchain m_menuSwapchain{XR_NULL_HANDLE};
        std::vector<std::shared_ptr<graphics::ITexture>> m_menuSwapchainImages;
        std::shared_ptr<menu::IMenuHandler> m_menuHandler;
        int m_menuLingering{0};
        bool m_requestScreenShotKeyState{false};

        struct {
            std::shared_ptr<utilities::ICpuTimer> appCpuTimer;
            std::shared_ptr<utilities::ICpuTimer> renderCpuTimer;
            std::shared_ptr<graphics::IGpuTimer> appGpuTimer[GpuTimerLatency + 1];
            std::shared_ptr<utilities::ICpuTimer> waitCpuTimer;
            std::shared_ptr<utilities::ICpuTimer> endFrameCpuTimer;
            std::shared_ptr<utilities::ICpuTimer> overlayCpuTimer;
            std::shared_ptr<graphics::IGpuTimer> overlayGpuTimer[GpuTimerLatency + 1];
            std::shared_ptr<utilities::ICpuTimer> handTrackingTimer;

            unsigned int gpuTimerIndex{0};
            std::chrono::steady_clock::time_point lastWindowStart;
            std::deque<std::pair<std::chrono::steady_clock::duration, uint32_t>> frameRates;
            uint32_t framesInPeriod{0};
            std::chrono::steady_clock::duration timePeriod{0s};
            uint32_t numFrames{0};
        } m_performanceCounters;

        menu::MenuStatistics m_stats{};
        std::ofstream m_logStats;
        bool m_hasPerformanceCounterKHR{false};
        bool m_hasVisibilityMaskKHR{false};
    };

    std::unique_ptr<OpenXrLayer> g_instance = nullptr;

} // namespace

namespace toolkit {
    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

    void ResetInstance() {
        g_instance.reset();
    }

} // namespace toolkit

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        TraceLoggingRegister(g_traceProvider);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
