/*
 * Copyright (C) 2024 The LineageOS Project
 *               2024 Paranoid Android
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android-base/logging.h>
#include <cutils/properties.h>

#include "Fingerprint.h"

namespace {

typedef struct fingerprint_hal {
    const char* id_name;
    const char* class_name;
    FingerprintSensorType sensor_type;
} fingerprint_hal_t;

static const fingerprint_hal_t kModules[] = {
    {"fingerprint.goodix_fod", NULL, FingerprintSensorType::UNDER_DISPLAY_OPTICAL},
    {"fingerprint", NULL, FingerprintSensorType::UNDER_DISPLAY_OPTICAL},
};

}  // anonymous namespace

namespace aidl {
namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {

namespace {
constexpr int SENSOR_ID = 0;
constexpr common::SensorStrength SENSOR_STRENGTH = common::SensorStrength::STRONG;
constexpr int MAX_ENROLLMENTS_PER_USER = 7;
constexpr bool SUPPORTS_NAVIGATION_GESTURES = false;
constexpr char HW_COMPONENT_ID[] = "fingerprintSensor";
constexpr char HW_VERSION[] = "vendor/model/revision";
constexpr char FW_VERSION[] = "1.01";
constexpr char SERIAL_NUMBER[] = "00000001";
constexpr char SW_COMPONENT_ID[] = "matchingAlgorithm";
constexpr char SW_VERSION[] = "vendor/version/revision";

}  // namespace

static const uint16_t kVersion = HARDWARE_MODULE_API_VERSION(2, 1);
static Fingerprint* sInstance;

Fingerprint::Fingerprint()
    : mSensorType(FingerprintSensorType::UNKNOWN),
      mMaxEnrollmentsPerUser(MAX_ENROLLMENTS_PER_USER),
      mSupportsGestures(false),
      mDevice(nullptr),
      mUdfpsHandlerFactory(nullptr),
      mUdfpsHandler(nullptr) {
    sInstance = this;  // keep track of the most recent instance
    for (auto& [id_name, class_name, sensor_type] : kModules) {
        mDevice = openHal(id_name, class_name);
        if (!mDevice) {
            ALOGE("Can't open HAL module, id %s, class %s", id_name, class_name);
            continue;
        }

        ALOGI("Opened fingerprint HAL, id %s, class %s", id_name, class_name);
        mSensorType = sensor_type;
        break;
    }

    if (!mDevice) {
        ALOGE("Can't open any HAL module");
    }

    if (mSensorType == FingerprintSensorType::UNDER_DISPLAY_OPTICAL
        || mSensorType == FingerprintSensorType::UNDER_DISPLAY_ULTRASONIC) {
        mUdfpsHandlerFactory = getUdfpsHandlerFactory();
        if (!mUdfpsHandlerFactory) {
            ALOGE("Can't get UdfpsHandlerFactory");
        } else {
            mUdfpsHandler = mUdfpsHandlerFactory->create();
            if (!mUdfpsHandler) {
                ALOGE("Can't create UdfpsHandler");
            } else {
                mUdfpsHandler->init(mDevice);
            }
        }
    }
}

Fingerprint::~Fingerprint() {
    ALOGV("~Fingerprint()");
    if (mUdfpsHandler) {
        mUdfpsHandlerFactory->destroy(mUdfpsHandler);
    }
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return;
    }
    int err;
    if (0 != (err = mDevice->common.close(reinterpret_cast<hw_device_t*>(mDevice)))) {
        ALOGE("Can't close fingerprint module, error: %d", err);
        return;
    }
    mDevice = nullptr;
}

fingerprint_device_t* Fingerprint::openHal(const char* id_name, const char* class_name) {
    int err;
    const hw_module_t* hw_mdl = nullptr;
    ALOGD("Opening fingerprint hal library...");
    if (0 != (err = hw_get_module_by_class(id_name, class_name, &hw_mdl)))  {
        ALOGE("Can't open fingerprint HW Module, error: %d", err);
        return nullptr;
    }

    if (hw_mdl == nullptr) {
        ALOGE("No valid fingerprint module");
        return nullptr;
    }

    fingerprint_module_t const* module = reinterpret_cast<const fingerprint_module_t*>(hw_mdl);
    if (module->common.methods->open == nullptr) {
        ALOGE("No valid open method");
        return nullptr;
    }

    hw_device_t* device = nullptr;

    if (0 != (err = module->common.methods->open(hw_mdl, nullptr, &device))) {
        ALOGE("Can't open fingerprint methods, error: %d", err);
        return nullptr;
    }

    if (kVersion != device->version) {
        // enforce version on new devices because of HIDL@2.1 translation layer
        ALOGE("Wrong fp version. Expected %d, got %d", kVersion, device->version);
        return nullptr;
    }

    fingerprint_device_t* fp_device = reinterpret_cast<fingerprint_device_t*>(device);

    if (0 != (err = fp_device->set_notify(fp_device, Fingerprint::notify))) {
        ALOGE("Can't register fingerprint module callback, error: %d", err);
        return nullptr;
    }

    return fp_device;
}

void Fingerprint::notify(const fingerprint_msg_t* msg) {
    Fingerprint* thisPtr = sInstance;
    if (thisPtr == nullptr || thisPtr->mSession == nullptr || thisPtr->mSession->isClosed()) {
        ALOGE("Receiving callbacks before a session is opened.");
        return;
    }
    thisPtr->mSession->notify(msg);
}

ndk::ScopedAStatus Fingerprint::getSensorProps(std::vector<SensorProps>* out) {
    std::vector<common::ComponentInfo> componentInfo = {
            {HW_COMPONENT_ID, HW_VERSION, FW_VERSION, SERIAL_NUMBER, "" /* softwareVersion */},
            {SW_COMPONENT_ID, "" /* hardwareVersion */, "" /* firmwareVersion */,
            "" /* serialNumber */, SW_VERSION}};
    common::CommonProps commonProps = {SENSOR_ID, SENSOR_STRENGTH,
                                       mMaxEnrollmentsPerUser, componentInfo};

    SensorLocation sensorLocation;

    int32_t x = property_get_int32("ro.vendor.feature.fingerprint_sensorui_position_center_x", -1);
    int32_t y = property_get_int32("ro.vendor.feature.fingerprint_sensorui_position_center_y", -1);
    int32_t r = property_get_int32("ro.vendor.feature.fingerprint_sensorui_position_center_r", -1);

    if (x >= 0 && y >= 0 && r >= 0) {
        sensorLocation.sensorLocationX = x;
        sensorLocation.sensorLocationY = y;
        sensorLocation.sensorRadius = r;
    } else {
        ALOGE("Failed to get sensor location: %d, %d, %d", x, y, r);
    }
    ALOGI("Sensor type: %s, location: %s", ::android::internal::ToString(mSensorType).c_str(), sensorLocation.toString().c_str());

    *out = {{commonProps,
             mSensorType,
             {sensorLocation},
             mSupportsGestures,
             false,
             false,
             false,
             std::nullopt}};

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Fingerprint::createSession(int32_t /*sensorId*/, int32_t userId,
                                              const std::shared_ptr<ISessionCallback>& cb,
                                              std::shared_ptr<ISession>* out) {
    CHECK(mSession == nullptr || mSession->isClosed()) << "Open session already exists!";

    mSession = SharedRefBase::make<Session>(mDevice, mUdfpsHandler, userId, cb, mLockoutTracker);
    *out = mSession;

    mSession->linkToDeath(cb->asBinder().get());

    return ndk::ScopedAStatus::ok();
}

} // namespace fingerprint
} // namespace biometrics
} // namespace hardware
} // namespace android
} // namespace aidl
