/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "InputReader"

//#define LOG_NDEBUG 0

// Log debug messages for each raw event received from the EventHub.
#define DEBUG_RAW_EVENTS 0

// Log debug messages about touch screen filtering hacks.
#define DEBUG_HACKS 0

// Log debug messages about virtual key processing.
#define DEBUG_VIRTUAL_KEYS 0

// Log debug messages about pointers.
#define DEBUG_POINTERS 0

// Log debug messages about pointer assignment calculations.
#define DEBUG_POINTER_ASSIGNMENT 0

// Log debug messages about gesture detection.
#define DEBUG_GESTURES 0

// Log debug messages about the vibrator.
#define DEBUG_VIBRATOR 0

#include "InputReader.h"

#include <cutils/log.h>
#include <androidfw/Keyboard.h>
#include <androidfw/VirtualKeyMap.h>

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

#define INDENT "  "
#define INDENT2 "    "
#define INDENT3 "      "
#define INDENT4 "        "
#define INDENT5 "          "

namespace android {

// --- Constants ---

// Maximum number of slots supported when using the slot-based Multitouch Protocol B.
static const size_t MAX_SLOTS = 32;

// --- Static Functions ---

template<typename T>
inline static T abs(const T& value) {
    return value < 0 ? - value : value;
}

template<typename T>
inline static T min(const T& a, const T& b) {
    return a < b ? a : b;
}

template<typename T>
inline static void swap(T& a, T& b) {
    T temp = a;
    a = b;
    b = temp;
}

inline static float avg(float x, float y) {
    return (x + y) / 2;
}

inline static float distance(float x1, float y1, float x2, float y2) {
    return hypotf(x1 - x2, y1 - y2);
}

inline static int32_t signExtendNybble(int32_t value) {
    return value >= 8 ? value - 16 : value;
}

static inline const char* toString(bool value) {
    return value ? "true" : "false";
}

static int32_t rotateValueUsingRotationMap(int32_t value, int32_t orientation,
        const int32_t map[][4], size_t mapSize) {
    if (orientation != DISPLAY_ORIENTATION_0) {
        for (size_t i = 0; i < mapSize; i++) {
            if (value == map[i][0]) {
                return map[i][orientation];
            }
        }
    }
    return value;
}

static const int32_t keyCodeRotationMap[][4] = {
        // key codes enumerated counter-clockwise with the original (unrotated) key first
        // no rotation,        90 degree rotation,  180 degree rotation, 270 degree rotation
        { AKEYCODE_DPAD_DOWN,   AKEYCODE_DPAD_RIGHT,  AKEYCODE_DPAD_UP,     AKEYCODE_DPAD_LEFT },
        { AKEYCODE_DPAD_RIGHT,  AKEYCODE_DPAD_UP,     AKEYCODE_DPAD_LEFT,   AKEYCODE_DPAD_DOWN },
        { AKEYCODE_DPAD_UP,     AKEYCODE_DPAD_LEFT,   AKEYCODE_DPAD_DOWN,   AKEYCODE_DPAD_RIGHT },
        { AKEYCODE_DPAD_LEFT,   AKEYCODE_DPAD_DOWN,   AKEYCODE_DPAD_RIGHT,  AKEYCODE_DPAD_UP },
};
static const size_t keyCodeRotationMapSize =
        sizeof(keyCodeRotationMap) / sizeof(keyCodeRotationMap[0]);

static int32_t rotateKeyCode(int32_t keyCode, int32_t orientation) {
    return rotateValueUsingRotationMap(keyCode, orientation,
            keyCodeRotationMap, keyCodeRotationMapSize);
}

static void rotateDelta(int32_t orientation, float* deltaX, float* deltaY) {
    float temp;
    switch (orientation) {
    case DISPLAY_ORIENTATION_90:
        temp = *deltaX;
        *deltaX = *deltaY;
        *deltaY = -temp;
        break;

    case DISPLAY_ORIENTATION_180:
        *deltaX = -*deltaX;
        *deltaY = -*deltaY;
        break;

    case DISPLAY_ORIENTATION_270:
        temp = *deltaX;
        *deltaX = -*deltaY;
        *deltaY = temp;
        break;
    }
}

static inline bool sourcesMatchMask(uint32_t sources, uint32_t sourceMask) {
    return (sources & sourceMask & ~ AINPUT_SOURCE_CLASS_MASK) != 0;
}

// Returns true if the pointer should be reported as being down given the specified
// button states.  This determines whether the event is reported as a touch event.
static bool isPointerDown(int32_t buttonState) {
    return buttonState &
            (AMOTION_EVENT_BUTTON_PRIMARY | AMOTION_EVENT_BUTTON_SECONDARY
                    | AMOTION_EVENT_BUTTON_TERTIARY);
}

static float calculateCommonVector(float a, float b) {
    if (a > 0 && b > 0) {
        return a < b ? a : b;
    } else if (a < 0 && b < 0) {
        return a > b ? a : b;
    } else {
        return 0;
    }
}

static void synthesizeButtonKey(InputReaderContext* context, int32_t action,
        nsecs_t when, int32_t deviceId, uint32_t source,
        uint32_t policyFlags, int32_t lastButtonState, int32_t currentButtonState,
        int32_t buttonState, int32_t keyCode) {
    if (
            (action == AKEY_EVENT_ACTION_DOWN
                    && !(lastButtonState & buttonState)
                    && (currentButtonState & buttonState))
            || (action == AKEY_EVENT_ACTION_UP
                    && (lastButtonState & buttonState)
                    && !(currentButtonState & buttonState))) {
        NotifyKeyArgs args(when, deviceId, source, policyFlags,
                action, 0, keyCode, 0, context->getGlobalMetaState(), when);
        context->getListener()->notifyKey(&args);
    }
}

static void synthesizeButtonKeys(InputReaderContext* context, int32_t action,
        nsecs_t when, int32_t deviceId, uint32_t source,
        uint32_t policyFlags, int32_t lastButtonState, int32_t currentButtonState) {
    synthesizeButtonKey(context, action, when, deviceId, source, policyFlags,
            lastButtonState, currentButtonState,
            AMOTION_EVENT_BUTTON_BACK, AKEYCODE_BACK);
    synthesizeButtonKey(context, action, when, deviceId, source, policyFlags,
            lastButtonState, currentButtonState,
            AMOTION_EVENT_BUTTON_FORWARD, AKEYCODE_FORWARD);
}


// --- InputReaderConfiguration ---

bool InputReaderConfiguration::getDisplayInfo(int32_t displayId, bool external,
        int32_t* width, int32_t* height, int32_t* orientation) const {
    if (displayId == 0) {
        const DisplayInfo& info = external ? mExternalDisplay : mInternalDisplay;
        if (info.width > 0 && info.height > 0) {
            if (width) {
                *width = info.width;
            }
            if (height) {
                *height = info.height;
            }
            if (orientation) {
                *orientation = info.orientation;
            }
            return true;
        }
    }
    return false;
}

void InputReaderConfiguration::setDisplayInfo(int32_t displayId, bool external,
        int32_t width, int32_t height, int32_t orientation) {
    if (displayId == 0) {
        DisplayInfo& info = external ? mExternalDisplay : mInternalDisplay;
        info.width = width;
        info.height = height;
        info.orientation = orientation;
    }
}


// --- InputReader ---

InputReader::InputReader(const sp<EventHubInterface>& eventHub,
        const sp<InputReaderPolicyInterface>& policy,
        const sp<InputListenerInterface>& listener) :
        mContext(this), mEventHub(eventHub), mPolicy(policy),
        mGlobalMetaState(0), mGeneration(1),
        mDisableVirtualKeysTimeout(LLONG_MIN), mNextTimeout(LLONG_MAX),
        mConfigurationChangesToRefresh(0) {
    mQueuedListener = new QueuedInputListener(listener);

    { // acquire lock
        AutoMutex _l(mLock);

        refreshConfigurationLocked(0);
        updateGlobalMetaStateLocked();
    } // release lock
}

InputReader::~InputReader() {
    for (size_t i = 0; i < mDevices.size(); i++) {
        delete mDevices.valueAt(i);
    }
}

void InputReader::loopOnce() {
    int32_t oldGeneration;
    int32_t timeoutMillis;
    bool inputDevicesChanged = false;
    Vector<InputDeviceInfo> inputDevices;
    { // acquire lock
        AutoMutex _l(mLock);

        oldGeneration = mGeneration;
        timeoutMillis = -1;

        uint32_t changes = mConfigurationChangesToRefresh;
        if (changes) {
            mConfigurationChangesToRefresh = 0;
            timeoutMillis = 0;
            refreshConfigurationLocked(changes);
        } else if (mNextTimeout != LLONG_MAX) {
            nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
            timeoutMillis = toMillisecondTimeoutDelay(now, mNextTimeout);
        }
    } // release lock

    size_t count = mEventHub->getEvents(timeoutMillis, mEventBuffer, EVENT_BUFFER_SIZE);

    { // acquire lock
        AutoMutex _l(mLock);
        mReaderIsAliveCondition.broadcast();

        if (count) {
            processEventsLocked(mEventBuffer, count);
        }

        if (mNextTimeout != LLONG_MAX) {
            nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
            if (now >= mNextTimeout) {
#if DEBUG_RAW_EVENTS
                ALOGD("Timeout expired, latency=%0.3fms", (now - mNextTimeout) * 0.000001f);
#endif
                mNextTimeout = LLONG_MAX;
                timeoutExpiredLocked(now);
            }
        }

        if (oldGeneration != mGeneration) {
            inputDevicesChanged = true;
            getInputDevicesLocked(inputDevices);
        }
    } // release lock

    // Send out a message that the describes the changed input devices.
    if (inputDevicesChanged) {
        mPolicy->notifyInputDevicesChanged(inputDevices);
    }

    // Flush queued events out to the listener.
    // This must happen outside of the lock because the listener could potentially call
    // back into the InputReader's methods, such as getScanCodeState, or become blocked
    // on another thread similarly waiting to acquire the InputReader lock thereby
    // resulting in a deadlock.  This situation is actually quite plausible because the
    // listener is actually the input dispatcher, which calls into the window manager,
    // which occasionally calls into the input reader.
    mQueuedListener->flush();
}

void InputReader::processEventsLocked(const RawEvent* rawEvents, size_t count) {
    for (const RawEvent* rawEvent = rawEvents; count;) {
        int32_t type = rawEvent->type;
        size_t batchSize = 1;
        if (type < EventHubInterface::FIRST_SYNTHETIC_EVENT) {
            int32_t deviceId = rawEvent->deviceId;
            while (batchSize < count) {
                if (rawEvent[batchSize].type >= EventHubInterface::FIRST_SYNTHETIC_EVENT
                        || rawEvent[batchSize].deviceId != deviceId) {
                    break;
                }
                batchSize += 1;
            }
#if DEBUG_RAW_EVENTS
            ALOGD("BatchSize: %d Count: %d", batchSize, count);
#endif
            processEventsForDeviceLocked(deviceId, rawEvent, batchSize);
        } else {
            switch (rawEvent->type) {
            case EventHubInterface::DEVICE_ADDED:
                addDeviceLocked(rawEvent->when, rawEvent->deviceId);
                break;
            case EventHubInterface::DEVICE_REMOVED:
                removeDeviceLocked(rawEvent->when, rawEvent->deviceId);
                break;
            case EventHubInterface::FINISHED_DEVICE_SCAN:
                handleConfigurationChangedLocked(rawEvent->when);
                break;
            default:
                ALOG_ASSERT(false); // can't happen
                break;
            }
        }
        count -= batchSize;
        rawEvent += batchSize;
    }
}

void InputReader::addDeviceLocked(nsecs_t when, int32_t deviceId) {
    ssize_t deviceIndex = mDevices.indexOfKey(deviceId);
    if (deviceIndex >= 0) {
        ALOGW("Ignoring spurious device added event for deviceId %d.", deviceId);
        return;
    }

    InputDeviceIdentifier identifier = mEventHub->getDeviceIdentifier(deviceId);
    uint32_t classes = mEventHub->getDeviceClasses(deviceId);

    InputDevice* device = createDeviceLocked(deviceId, identifier, classes);
    device->configure(when, &mConfig, 0);
    device->reset(when);

    if (device->isIgnored()) {
        ALOGI("Device added: id=%d, name='%s' (ignored non-input device)", deviceId,
                identifier.name.string());
    } else {
        ALOGI("Device added: id=%d, name='%s', sources=0x%08x", deviceId,
                identifier.name.string(), device->getSources());
    }

    mDevices.add(deviceId, device);
    bumpGenerationLocked();
}

void InputReader::removeDeviceLocked(nsecs_t when, int32_t deviceId) {
    InputDevice* device = NULL;
    ssize_t deviceIndex = mDevices.indexOfKey(deviceId);
    if (deviceIndex < 0) {
        ALOGW("Ignoring spurious device removed event for deviceId %d.", deviceId);
        return;
    }

    device = mDevices.valueAt(deviceIndex);
    mDevices.removeItemsAt(deviceIndex, 1);
    bumpGenerationLocked();

    if (device->isIgnored()) {
        ALOGI("Device removed: id=%d, name='%s' (ignored non-input device)",
                device->getId(), device->getName().string());
    } else {
        ALOGI("Device removed: id=%d, name='%s', sources=0x%08x",
                device->getId(), device->getName().string(), device->getSources());
    }

    device->reset(when);
    delete device;
}

InputDevice* InputReader::createDeviceLocked(int32_t deviceId,
        const InputDeviceIdentifier& identifier, uint32_t classes) {
    InputDevice* device = new InputDevice(&mContext, deviceId, bumpGenerationLocked(),
            identifier, classes);

    // External devices.
    if (classes & INPUT_DEVICE_CLASS_EXTERNAL) {
        device->setExternal(true);
    }

    // Switch-like devices.
    if (classes & INPUT_DEVICE_CLASS_SWITCH) {
        device->addMapper(new SwitchInputMapper(device));
    }

    // Vibrator-like devices.
    if (classes & INPUT_DEVICE_CLASS_VIBRATOR) {
        device->addMapper(new VibratorInputMapper(device));
    }

    // Keyboard-like devices.
    uint32_t keyboardSource = 0;
    int32_t keyboardType = AINPUT_KEYBOARD_TYPE_NON_ALPHABETIC;
    if (classes & INPUT_DEVICE_CLASS_KEYBOARD) {
        keyboardSource |= AINPUT_SOURCE_KEYBOARD;
    }
    if (classes & INPUT_DEVICE_CLASS_ALPHAKEY) {
        keyboardType = AINPUT_KEYBOARD_TYPE_ALPHABETIC;
    }
    if (classes & INPUT_DEVICE_CLASS_DPAD) {
        keyboardSource |= AINPUT_SOURCE_DPAD;
    }
    if (classes & INPUT_DEVICE_CLASS_GAMEPAD) {
        keyboardSource |= AINPUT_SOURCE_GAMEPAD;
    }

    if (keyboardSource != 0) {
        device->addMapper(new KeyboardInputMapper(device, keyboardSource, keyboardType));
    }

    // Cursor-like devices.
    if (classes & INPUT_DEVICE_CLASS_CURSOR) {
        device->addMapper(new CursorInputMapper(device));
    }

    // Touchscreens and touchpad devices.
    if (classes & INPUT_DEVICE_CLASS_TOUCH_MT) {
        device->addMapper(new MultiTouchInputMapper(device));
    } else if (classes & INPUT_DEVICE_CLASS_TOUCH) {
        device->addMapper(new SingleTouchInputMapper(device));
    }

    // Joystick-like devices.
    if (classes & INPUT_DEVICE_CLASS_JOYSTICK) {
        device->addMapper(new JoystickInputMapper(device));
    }

    return device;
}

void InputReader::processEventsForDeviceLocked(int32_t deviceId,
        const RawEvent* rawEvents, size_t count) {
    ssize_t deviceIndex = mDevices.indexOfKey(deviceId);
    if (deviceIndex < 0) {
        ALOGW("Discarding event for unknown deviceId %d.", deviceId);
        return;
    }

    InputDevice* device = mDevices.valueAt(deviceIndex);
    if (device->isIgnored()) {
        //ALOGD("Discarding event for ignored deviceId %d.", deviceId);
        return;
    }

    device->process(rawEvents, count);
}

void InputReader::timeoutExpiredLocked(nsecs_t when) {
    for (size_t i = 0; i < mDevices.size(); i++) {
        InputDevice* device = mDevices.valueAt(i);
        if (!device->isIgnored()) {
            device->timeoutExpired(when);
        }
    }
}

void InputReader::handleConfigurationChangedLocked(nsecs_t when) {
    // Reset global meta state because it depends on the list of all configured devices.
    updateGlobalMetaStateLocked();

    // Enqueue configuration changed.
    NotifyConfigurationChangedArgs args(when);
    mQueuedListener->notifyConfigurationChanged(&args);
}

void InputReader::refreshConfigurationLocked(uint32_t changes) {
    mPolicy->getReaderConfiguration(&mConfig);
    mEventHub->setExcludedDevices(mConfig.excludedDeviceNames);

    if (changes) {
        ALOGI("Reconfiguring input devices.  changes=0x%08x", changes);
        nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);

        if (changes & InputReaderConfiguration::CHANGE_MUST_REOPEN) {
            mEventHub->requestReopenDevices();
        } else {
            for (size_t i = 0; i < mDevices.size(); i++) {
                InputDevice* device = mDevices.valueAt(i);
                device->configure(now, &mConfig, changes);
            }
        }
    }
}

void InputReader::updateGlobalMetaStateLocked() {
    mGlobalMetaState = 0;

    for (size_t i = 0; i < mDevices.size(); i++) {
        InputDevice* device = mDevices.valueAt(i);
        mGlobalMetaState |= device->getMetaState();
    }
}

int32_t InputReader::getGlobalMetaStateLocked() {
    return mGlobalMetaState;
}

void InputReader::disableVirtualKeysUntilLocked(nsecs_t time) {
    mDisableVirtualKeysTimeout = time;
}

bool InputReader::shouldDropVirtualKeyLocked(nsecs_t now,
        InputDevice* device, int32_t keyCode, int32_t scanCode) {
    if (now < mDisableVirtualKeysTimeout) {
        ALOGI("Dropping virtual key from device %s because virtual keys are "
                "temporarily disabled for the next %0.3fms.  keyCode=%d, scanCode=%d",
                device->getName().string(),
                (mDisableVirtualKeysTimeout - now) * 0.000001,
                keyCode, scanCode);
        return true;
    } else {
        return false;
    }
}

void InputReader::fadePointerLocked() {
    for (size_t i = 0; i < mDevices.size(); i++) {
        InputDevice* device = mDevices.valueAt(i);
        device->fadePointer();
    }
}

void InputReader::requestTimeoutAtTimeLocked(nsecs_t when) {
    if (when < mNextTimeout) {
        mNextTimeout = when;
        mEventHub->wake();
    }
}

int32_t InputReader::bumpGenerationLocked() {
    return ++mGeneration;
}

void InputReader::getInputDevices(Vector<InputDeviceInfo>& outInputDevices) {
    AutoMutex _l(mLock);
    getInputDevicesLocked(outInputDevices);
}

void InputReader::getInputDevicesLocked(Vector<InputDeviceInfo>& outInputDevices) {
    outInputDevices.clear();

    size_t numDevices = mDevices.size();
    for (size_t i = 0; i < numDevices; i++) {
        InputDevice* device = mDevices.valueAt(i);
        if (!device->isIgnored()) {
            outInputDevices.push();
            device->getDeviceInfo(&outInputDevices.editTop());
        }
    }
}

int32_t InputReader::getKeyCodeState(int32_t deviceId, uint32_t sourceMask,
        int32_t keyCode) {
    AutoMutex _l(mLock);

    return getStateLocked(deviceId, sourceMask, keyCode, &InputDevice::getKeyCodeState);
}

int32_t InputReader::getScanCodeState(int32_t deviceId, uint32_t sourceMask,
        int32_t scanCode) {
    AutoMutex _l(mLock);

    return getStateLocked(deviceId, sourceMask, scanCode, &InputDevice::getScanCodeState);
}

int32_t InputReader::getSwitchState(int32_t deviceId, uint32_t sourceMask, int32_t switchCode) {
    AutoMutex _l(mLock);

    return getStateLocked(deviceId, sourceMask, switchCode, &InputDevice::getSwitchState);
}

int32_t InputReader::getStateLocked(int32_t deviceId, uint32_t sourceMask, int32_t code,
        GetStateFunc getStateFunc) {
    int32_t result = AKEY_STATE_UNKNOWN;
    if (deviceId >= 0) {
        ssize_t deviceIndex = mDevices.indexOfKey(deviceId);
        if (deviceIndex >= 0) {
            InputDevice* device = mDevices.valueAt(deviceIndex);
            if (! device->isIgnored() && sourcesMatchMask(device->getSources(), sourceMask)) {
                result = (device->*getStateFunc)(sourceMask, code);
            }
        }
    } else {
        size_t numDevices = mDevices.size();
        for (size_t i = 0; i < numDevices; i++) {
            InputDevice* device = mDevices.valueAt(i);
            if (! device->isIgnored() && sourcesMatchMask(device->getSources(), sourceMask)) {
                // If any device reports AKEY_STATE_DOWN or AKEY_STATE_VIRTUAL, return that
                // value.  Otherwise, return AKEY_STATE_UP as long as one device reports it.
                int32_t currentResult = (device->*getStateFunc)(sourceMask, code);
                if (currentResult >= AKEY_STATE_DOWN) {
                    return currentResult;
                } else if (currentResult == AKEY_STATE_UP) {
                    result = currentResult;
                }
            }
        }
    }
    return result;
}

bool InputReader::hasKeys(int32_t deviceId, uint32_t sourceMask,
        size_t numCodes, const int32_t* keyCodes, uint8_t* outFlags) {
    AutoMutex _l(mLock);

    memset(outFlags, 0, numCodes);
    return markSupportedKeyCodesLocked(deviceId, sourceMask, numCodes, keyCodes, outFlags);
}

bool InputReader::markSupportedKeyCodesLocked(int32_t deviceId, uint32_t sourceMask,
        size_t numCodes, const int32_t* keyCodes, uint8_t* outFlags) {
    bool result = false;
    if (deviceId >= 0) {
        ssize_t deviceIndex = mDevices.indexOfKey(deviceId);
        if (deviceIndex >= 0) {
            InputDevice* device = mDevices.valueAt(deviceIndex);
            if (! device->isIgnored() && sourcesMatchMask(device->getSources(), sourceMask)) {
                result = device->markSupportedKeyCodes(sourceMask,
                        numCodes, keyCodes, outFlags);
            }
        }
    } else {
        size_t numDevices = mDevices.size();
        for (size_t i = 0; i < numDevices; i++) {
            InputDevice* device = mDevices.valueAt(i);
            if (! device->isIgnored() && sourcesMatchMask(device->getSources(), sourceMask)) {
                result |= device->markSupportedKeyCodes(sourceMask,
                        numCodes, keyCodes, outFlags);
            }
        }
    }
    return result;
}

void InputReader::requestRefreshConfiguration(uint32_t changes) {
    AutoMutex _l(mLock);

    if (changes) {
        bool needWake = !mConfigurationChangesToRefresh;
        mConfigurationChangesToRefresh |= changes;

        if (needWake) {
            mEventHub->wake();
        }
    }
}

void InputReader::vibrate(int32_t deviceId, const nsecs_t* pattern, size_t patternSize,
        ssize_t repeat, int32_t token) {
    AutoMutex _l(mLock);

    ssize_t deviceIndex = mDevices.indexOfKey(deviceId);
    if (deviceIndex >= 0) {
        InputDevice* device = mDevices.valueAt(deviceIndex);
        device->vibrate(pattern, patternSize, repeat, token);
    }
}

void InputReader::cancelVibrate(int32_t deviceId, int32_t token) {
    AutoMutex _l(mLock);

    ssize_t deviceIndex = mDevices.indexOfKey(deviceId);
    if (deviceIndex >= 0) {
        InputDevice* device = mDevices.valueAt(deviceIndex);
        device->cancelVibrate(token);
    }
}

void InputReader::dump(String8& dump) {
    AutoMutex _l(mLock);

    mEventHub->dump(dump);
    dump.append("\n");

    dump.append("Input Reader State:\n");

    for (size_t i = 0; i < mDevices.size(); i++) {
        mDevices.valueAt(i)->dump(dump);
    }

    dump.append(INDENT "Configuration:\n");
    dump.append(INDENT2 "ExcludedDeviceNames: [");
    for (size_t i = 0; i < mConfig.excludedDeviceNames.size(); i++) {
        if (i != 0) {
            dump.append(", ");
        }
        dump.append(mConfig.excludedDeviceNames.itemAt(i).string());
    }
    dump.append("]\n");
    dump.appendFormat(INDENT2 "VirtualKeyQuietTime: %0.1fms\n",
            mConfig.virtualKeyQuietTime * 0.000001f);

    dump.appendFormat(INDENT2 "PointerVelocityControlParameters: "
            "scale=%0.3f, lowThreshold=%0.3f, highThreshold=%0.3f, acceleration=%0.3f\n",
            mConfig.pointerVelocityControlParameters.scale,
            mConfig.pointerVelocityControlParameters.lowThreshold,
            mConfig.pointerVelocityControlParameters.highThreshold,
            mConfig.pointerVelocityControlParameters.acceleration);

    dump.appendFormat(INDENT2 "WheelVelocityControlParameters: "
            "scale=%0.3f, lowThreshold=%0.3f, highThreshold=%0.3f, acceleration=%0.3f\n",
            mConfig.wheelVelocityControlParameters.scale,
            mConfig.wheelVelocityControlParameters.lowThreshold,
            mConfig.wheelVelocityControlParameters.highThreshold,
            mConfig.wheelVelocityControlParameters.acceleration);

    dump.appendFormat(INDENT2 "PointerGesture:\n");
    dump.appendFormat(INDENT3 "Enabled: %s\n",
            toString(mConfig.pointerGesturesEnabled));
    dump.appendFormat(INDENT3 "QuietInterval: %0.1fms\n",
            mConfig.pointerGestureQuietInterval * 0.000001f);
    dump.appendFormat(INDENT3 "DragMinSwitchSpeed: %0.1fpx/s\n",
            mConfig.pointerGestureDragMinSwitchSpeed);
    dump.appendFormat(INDENT3 "TapInterval: %0.1fms\n",
            mConfig.pointerGestureTapInterval * 0.000001f);
    dump.appendFormat(INDENT3 "TapDragInterval: %0.1fms\n",
            mConfig.pointerGestureTapDragInterval * 0.000001f);
    dump.appendFormat(INDENT3 "TapSlop: %0.1fpx\n",
            mConfig.pointerGestureTapSlop);
    dump.appendFormat(INDENT3 "MultitouchSettleInterval: %0.1fms\n",
            mConfig.pointerGestureMultitouchSettleInterval * 0.000001f);
    dump.appendFormat(INDENT3 "MultitouchMinDistance: %0.1fpx\n",
            mConfig.pointerGestureMultitouchMinDistance);
    dump.appendFormat(INDENT3 "SwipeTransitionAngleCosine: %0.1f\n",
            mConfig.pointerGestureSwipeTransitionAngleCosine);
    dump.appendFormat(INDENT3 "SwipeMaxWidthRatio: %0.1f\n",
            mConfig.pointerGestureSwipeMaxWidthRatio);
    dump.appendFormat(INDENT3 "MovementSpeedRatio: %0.1f\n",
            mConfig.pointerGestureMovementSpeedRatio);
    dump.appendFormat(INDENT3 "ZoomSpeedRatio: %0.1f\n",
            mConfig.pointerGestureZoomSpeedRatio);
}

void InputReader::monitor() {
    // Acquire and release the lock to ensure that the reader has not deadlocked.
    mLock.lock();
    mEventHub->wake();
    mReaderIsAliveCondition.wait(mLock);
    mLock.unlock();

    // Check the EventHub
    mEventHub->monitor();
}


// --- InputReader::ContextImpl ---

InputReader::ContextImpl::ContextImpl(InputReader* reader) :
        mReader(reader) {
}

void InputReader::ContextImpl::updateGlobalMetaState() {
    // lock is already held by the input loop
    mReader->updateGlobalMetaStateLocked();
}

int32_t InputReader::ContextImpl::getGlobalMetaState() {
    // lock is already held by the input loop
    return mReader->getGlobalMetaStateLocked();
}

void InputReader::ContextImpl::disableVirtualKeysUntil(nsecs_t time) {
    // lock is already held by the input loop
    mReader->disableVirtualKeysUntilLocked(time);
}

bool InputReader::ContextImpl::shouldDropVirtualKey(nsecs_t now,
        InputDevice* device, int32_t keyCode, int32_t scanCode) {
    // lock is already held by the input loop
    return mReader->shouldDropVirtualKeyLocked(now, device, keyCode, scanCode);
}

void InputReader::ContextImpl::fadePointer() {
    // lock is already held by the input loop
    mReader->fadePointerLocked();
}

void InputReader::ContextImpl::requestTimeoutAtTime(nsecs_t when) {
    // lock is already held by the input loop
    mReader->requestTimeoutAtTimeLocked(when);
}

int32_t InputReader::ContextImpl::bumpGeneration() {
    // lock is already held by the input loop
    return mReader->bumpGenerationLocked();
}

InputReaderPolicyInterface* InputReader::ContextImpl::getPolicy() {
    return mReader->mPolicy.get();
}

InputListenerInterface* InputReader::ContextImpl::getListener() {
    return mReader->mQueuedListener.get();
}

EventHubInterface* InputReader::ContextImpl::getEventHub() {
    return mReader->mEventHub.get();
}


// --- InputReaderThread ---

InputReaderThread::InputReaderThread(const sp<InputReaderInterface>& reader) :
        Thread(/*canCallJava*/ true), mReader(reader) {
}

InputReaderThread::~InputReaderThread() {
}

bool InputReaderThread::threadLoop() {
    mReader->loopOnce();
    return true;
}


// --- InputDevice ---

InputDevice::InputDevice(InputReaderContext* context, int32_t id, int32_t generation,
        const InputDeviceIdentifier& identifier, uint32_t classes) :
        mContext(context), mId(id), mGeneration(generation),
        mIdentifier(identifier), mClasses(classes),
        mSources(0), mIsExternal(false), mDropUntilNextSync(false) {
}

InputDevice::~InputDevice() {
    size_t numMappers = mMappers.size();
    for (size_t i = 0; i < numMappers; i++) {
        delete mMappers[i];
    }
    mMappers.clear();
}

void InputDevice::dump(String8& dump) {
    InputDeviceInfo deviceInfo;
    getDeviceInfo(& deviceInfo);

    dump.appendFormat(INDENT "Device %d: %s\n", deviceInfo.getId(),
            deviceInfo.getDisplayName().string());
    dump.appendFormat(INDENT2 "Generation: %d\n", mGeneration);
    dump.appendFormat(INDENT2 "IsExternal: %s\n", toString(mIsExternal));
    dump.appendFormat(INDENT2 "Sources: 0x%08x\n", deviceInfo.getSources());
    dump.appendFormat(INDENT2 "KeyboardType: %d\n", deviceInfo.getKeyboardType());

    const Vector<InputDeviceInfo::MotionRange>& ranges = deviceInfo.getMotionRanges();
    if (!ranges.isEmpty()) {
        dump.append(INDENT2 "Motion Ranges:\n");
        for (size_t i = 0; i < ranges.size(); i++) {
            const InputDeviceInfo::MotionRange& range = ranges.itemAt(i);
            const char* label = getAxisLabel(range.axis);
            char name[32];
            if (label) {
                strncpy(name, label, sizeof(name));
                name[sizeof(name) - 1] = '\0';
            } else {
                snprintf(name, sizeof(name), "%d", range.axis);
            }
            dump.appendFormat(INDENT3 "%s: source=0x%08x, "
                    "min=%0.3f, max=%0.3f, flat=%0.3f, fuzz=%0.3f\n",
                    name, range.source, range.min, range.max, range.flat, range.fuzz);
        }
    }

    size_t numMappers = mMappers.size();
    for (size_t i = 0; i < numMappers; i++) {
        InputMapper* mapper = mMappers[i];
        mapper->dump(dump);
    }
}

void InputDevice::addMapper(InputMapper* mapper) {
    mMappers.add(mapper);
}

void InputDevice::configure(nsecs_t when, const InputReaderConfiguration* config, uint32_t changes) {
    mSources = 0;

    if (!isIgnored()) {
        if (!changes) { // first time only
            mContext->getEventHub()->getConfiguration(mId, &mConfiguration);
        }

        if (!changes || (changes & InputReaderConfiguration::CHANGE_KEYBOARD_LAYOUTS)) {
            if (!(mClasses & INPUT_DEVICE_CLASS_VIRTUAL)) {
                sp<KeyCharacterMap> keyboardLayout =
                        mContext->getPolicy()->getKeyboardLayoutOverlay(mIdentifier.descriptor);
                if (mContext->getEventHub()->setKeyboardLayoutOverlay(mId, keyboardLayout)) {
                    bumpGeneration();
                }
            }
        }

        if (!changes || (changes & InputReaderConfiguration::CHANGE_DEVICE_ALIAS)) {
            if (!(mClasses & INPUT_DEVICE_CLASS_VIRTUAL)) {
                String8 alias = mContext->getPolicy()->getDeviceAlias(mIdentifier);
                if (mAlias != alias) {
                    mAlias = alias;
                    bumpGeneration();
                }
            }
        }

        size_t numMappers = mMappers.size();
        for (size_t i = 0; i < numMappers; i++) {
            InputMapper* mapper = mMappers[i];
            mapper->configure(when, config, changes);
            mSources |= mapper->getSources();
        }
    }
}

void InputDevice::reset(nsecs_t when) {
    size_t numMappers = mMappers.size();
    for (size_t i = 0; i < numMappers; i++) {
        InputMapper* mapper = mMappers[i];
        mapper->reset(when);
    }

    mContext->updateGlobalMetaState();

    notifyReset(when);
}

void InputDevice::process(const RawEvent* rawEvents, size_t count) {
    // Process all of the events in order for each mapper.
    // We cannot simply ask each mapper to process them in bulk because mappers may
    // have side-effects that must be interleaved.  For example, joystick movement events and
    // gamepad button presses are handled by different mappers but they should be dispatched
    // in the order received.
    size_t numMappers = mMappers.size();
    for (const RawEvent* rawEvent = rawEvents; count--; rawEvent++) {
#if DEBUG_RAW_EVENTS
        ALOGD("Input event: device=%d type=0x%04x code=0x%04x value=0x%08x",
                rawEvent->deviceId, rawEvent->type, rawEvent->code, rawEvent->value);
#endif

        if (mDropUntilNextSync) {
            if (rawEvent->type == EV_SYN && rawEvent->code == SYN_REPORT) {
                mDropUntilNextSync = false;
#if DEBUG_RAW_EVENTS
                ALOGD("Recovered from input event buffer overrun.");
#endif
            } else {
#if DEBUG_RAW_EVENTS
                ALOGD("Dropped input event while waiting for next input sync.");
#endif
            }

        } else if (rawEvent->type == EV_SYN && rawEvent->code == SYN_DROPPED) {
            ALOGI("Detected input event buffer overrun for device %s.", getName().string());
            mDropUntilNextSync = true;
            reset(rawEvent->when);
        } else {
            for (size_t i = 0; i < numMappers; i++) {
                InputMapper* mapper = mMappers[i];
                mapper->process(rawEvent);
            }
        }
    }
}

void InputDevice::timeoutExpired(nsecs_t when) {
    size_t numMappers = mMappers.size();
    for (size_t i = 0; i < numMappers; i++) {
        InputMapper* mapper = mMappers[i];
        mapper->timeoutExpired(when);
    }
}

void InputDevice::getDeviceInfo(InputDeviceInfo* outDeviceInfo) {
    outDeviceInfo->initialize(mId, mGeneration, mIdentifier, mAlias, mIsExternal);

    size_t numMappers = mMappers.size();
    for (size_t i = 0; i < numMappers; i++) {
        InputMapper* mapper = mMappers[i];
        mapper->populateDeviceInfo(outDeviceInfo);
    }
}

int32_t InputDevice::getKeyCodeState(uint32_t sourceMask, int32_t keyCode) {
    return getState(sourceMask, keyCode, & InputMapper::getKeyCodeState);
}

int32_t InputDevice::getScanCodeState(uint32_t sourceMask, int32_t scanCode) {
    return getState(sourceMask, scanCode, & InputMapper::getScanCodeState);
}

int32_t InputDevice::getSwitchState(uint32_t sourceMask, int32_t switchCode) {
    return getState(sourceMask, switchCode, & InputMapper::getSwitchState);
}

int32_t InputDevice::getState(uint32_t sourceMask, int32_t code, GetStateFunc getStateFunc) {
    int32_t result = AKEY_STATE_UNKNOWN;
    size_t numMappers = mMappers.size();
    for (size_t i = 0; i < numMappers; i++) {
        InputMapper* mapper = mMappers[i];
        if (sourcesMatchMask(mapper->getSources(), sourceMask)) {
            // If any mapper reports AKEY_STATE_DOWN or AKEY_STATE_VIRTUAL, return that
            // value.  Otherwise, return AKEY_STATE_UP as long as one mapper reports it.
            int32_t currentResult = (mapper->*getStateFunc)(sourceMask, code);
            if (currentResult >= AKEY_STATE_DOWN) {
                return currentResult;
            } else if (currentResult == AKEY_STATE_UP) {
                result = currentResult;
            }
        }
    }
    return result;
}

bool InputDevice::markSupportedKeyCodes(uint32_t sourceMask, size_t numCodes,
        const int32_t* keyCodes, uint8_t* outFlags) {
    bool result = false;
    size_t numMappers = mMappers.size();
    for (size_t i = 0; i < numMappers; i++) {
        InputMapper* mapper = mMappers[i];
        if (sourcesMatchMask(mapper->getSources(), sourceMask)) {
            result |= mapper->markSupportedKeyCodes(sourceMask, numCodes, keyCodes, outFlags);
        }
    }
    return result;
}

void InputDevice::vibrate(const nsecs_t* pattern, size_t patternSize, ssize_t repeat,
        int32_t token) {
    size_t numMappers = mMappers.size();
    for (size_t i = 0; i < numMappers; i++) {
        InputMapper* mapper = mMappers[i];
        mapper->vibrate(pattern, patternSize, repeat, token);
    }
}

void InputDevice::cancelVibrate(int32_t token) {
    size_t numMappers = mMappers.size();
    for (size_t i = 0; i < numMappers; i++) {
        InputMapper* mapper = mMappers[i];
        mapper->cancelVibrate(token);
    }
}

int32_t InputDevice::getMetaState() {
    int32_t result = 0;
    size_t numMappers = mMappers.size();
    for (size_t i = 0; i < numMappers; i++) {
        InputMapper* mapper = mMappers[i];
        result |= mapper->getMetaState();
    }
    return result;
}

void InputDevice::fadePointer() {
    size_t numMappers = mMappers.size();
    for (size_t i = 0; i < numMappers; i++) {
        InputMapper* mapper = mMappers[i];
        mapper->fadePointer();
    }
}

void InputDevice::bumpGeneration() {
    mGeneration = mContext->bumpGeneration();
}

void InputDevice::notifyReset(nsecs_t when) {
    NotifyDeviceResetArgs args(when, mId);
    mContext->getListener()->notifyDeviceReset(&args);
}


// --- CursorButtonAccumulator ---

CursorButtonAccumulator::CursorButtonAccumulator() {
    clearButtons();
}

void CursorButtonAccumulator::reset(InputDevice* device) {
    mBtnLeft = device->isKeyPressed(BTN_LEFT);
    mBtnRight = device->isKeyPressed(BTN_RIGHT);
    mBtnMiddle = device->isKeyPressed(BTN_MIDDLE);
    mBtnBack = device->isKeyPressed(BTN_BACK);
    mBtnSide = device->isKeyPressed(BTN_SIDE);
    mBtnForward = device->isKeyPressed(BTN_FORWARD);
    mBtnExtra = device->isKeyPressed(BTN_EXTRA);
    mBtnTask = device->isKeyPressed(BTN_TASK);
}

void CursorButtonAccumulator::clearButtons() {
    mBtnLeft = 0;
    mBtnRight = 0;
    mBtnMiddle = 0;
    mBtnBack = 0;
    mBtnSide = 0;
    mBtnForward = 0;
    mBtnExtra = 0;
    mBtnTask = 0;
}

void CursorButtonAccumulator::process(const RawEvent* rawEvent) {
    if (rawEvent->type == EV_KEY) {
        switch (rawEvent->code) {
        case BTN_LEFT:
            mBtnLeft = rawEvent->value;
            break;
        case BTN_RIGHT:
            mBtnRight = rawEvent->value;
            break;
        case BTN_MIDDLE:
            mBtnMiddle = rawEvent->value;
            break;
        case BTN_BACK:
            mBtnBack = rawEvent->value;
            break;
        case BTN_SIDE:
            mBtnSide = rawEvent->value;
            break;
        case BTN_FORWARD:
            mBtnForward = rawEvent->value;
            break;
        case BTN_EXTRA:
            mBtnExtra = rawEvent->value;
            break;
        case BTN_TASK:
            mBtnTask = rawEvent->value;
            break;
        }
    }
}

uint32_t CursorButtonAccumulator::getButtonState() const {
    uint32_t result = 0;
    if (mBtnLeft) {
        result |= AMOTION_EVENT_BUTTON_PRIMARY;
    }
    if (mBtnRight) {
        result |= AMOTION_EVENT_BUTTON_SECONDARY;
    }
    if (mBtnMiddle) {
        result |= AMOTION_EVENT_BUTTON_TERTIARY;
    }
    if (mBtnBack || mBtnSide) {
        result |= AMOTION_EVENT_BUTTON_BACK;
    }
    if (mBtnForward || mBtnExtra) {
        result |= AMOTION_EVENT_BUTTON_FORWARD;
    }
    return result;
}


// --- CursorMotionAccumulator ---

CursorMotionAccumulator::CursorMotionAccumulator() {
    clearRelativeAxes();
}

void CursorMotionAccumulator::reset(InputDevice* device) {
    clearRelativeAxes();
}

void CursorMotionAccumulator::clearRelativeAxes() {
    mRelX = 0;
    mRelY = 0;
}

void CursorMotionAccumulator::process(const RawEvent* rawEvent) {
    if (rawEvent->type == EV_REL) {
        switch (rawEvent->code) {
        case REL_X:
            mRelX = rawEvent->value;
            break;
        case REL_Y:
            mRelY = rawEvent->value;
            break;
        }
    }
}

void CursorMotionAccumulator::finishSync() {
    clearRelativeAxes();
}


// --- CursorScrollAccumulator ---

CursorScrollAccumulator::CursorScrollAccumulator() :
        mHaveRelWheel(false), mHaveRelHWheel(false) {
    clearRelativeAxes();
}

void CursorScrollAccumulator::configure(InputDevice* device) {
    mHaveRelWheel = device->getEventHub()->hasRelativeAxis(device->getId(), REL_WHEEL);
    mHaveRelHWheel = device->getEventHub()->hasRelativeAxis(device->getId(), REL_HWHEEL);
}

void CursorScrollAccumulator::reset(InputDevice* device) {
    clearRelativeAxes();
}

void CursorScrollAccumulator::clearRelativeAxes() {
    mRelWheel = 0;
    mRelHWheel = 0;
}

void CursorScrollAccumulator::process(const RawEvent* rawEvent) {
    if (rawEvent->type == EV_REL) {
        switch (rawEvent->code) {
        case REL_WHEEL:
            mRelWheel = rawEvent->value;
            break;
        case REL_HWHEEL:
            mRelHWheel = rawEvent->value;
            break;
        }
    }
}

void CursorScrollAccumulator::finishSync() {
    clearRelativeAxes();
}


// --- TouchButtonAccumulator ---

TouchButtonAccumulator::TouchButtonAccumulator() :
        mHaveBtnTouch(false), mHaveStylus(false) {
    clearButtons();
}

void TouchButtonAccumulator::configure(InputDevice* device) {
    mHaveBtnTouch = device->hasKey(BTN_TOUCH);
    mHaveStylus = device->hasKey(BTN_TOOL_PEN)
            || device->hasKey(BTN_TOOL_RUBBER)
            || device->hasKey(BTN_TOOL_BRUSH)
            || device->hasKey(BTN_TOOL_PENCIL)
            || device->hasKey(BTN_TOOL_AIRBRUSH);
}

void TouchButtonAccumulator::reset(InputDevice* device) {
    mBtnTouch = device->isKeyPressed(BTN_TOUCH);
    mBtnStylus = device->isKeyPressed(BTN_STYLUS);
    mBtnStylus2 = device->isKeyPressed(BTN_STYLUS);
    mBtnToolFinger = device->isKeyPressed(BTN_TOOL_FINGER);
    mBtnToolPen = device->isKeyPressed(BTN_TOOL_PEN);
    mBtnToolRubber = device->isKeyPressed(BTN_TOOL_RUBBER);
    mBtnToolBrush = device->isKeyPressed(BTN_TOOL_BRUSH);
    mBtnToolPencil = device->isKeyPressed(BTN_TOOL_PENCIL);
    mBtnToolAirbrush = device->isKeyPressed(BTN_TOOL_AIRBRUSH);
    mBtnToolMouse = device->isKeyPressed(BTN_TOOL_MOUSE);
    mBtnToolLens = device->isKeyPressed(BTN_TOOL_LENS);
    mBtnToolDoubleTap = device->isKeyPressed(BTN_TOOL_DOUBLETAP);
    mBtnToolTripleTap = device->isKeyPressed(BTN_TOOL_TRIPLETAP);
    mBtnToolQuadTap = device->isKeyPressed(BTN_TOOL_QUADTAP);
}

void TouchButtonAccumulator::clearButtons() {
    mBtnTouch = 0;
    mBtnStylus = 0;
    mBtnStylus2 = 0;
    mBtnToolFinger = 0;
    mBtnToolPen = 0;
    mBtnToolRubber = 0;
    mBtnToolBrush = 0;
    mBtnToolPencil = 0;
    mBtnToolAirbrush = 0;
    mBtnToolMouse = 0;
    mBtnToolLens = 0;
    mBtnToolDoubleTap = 0;
    mBtnToolTripleTap = 0;
    mBtnToolQuadTap = 0;
}

void TouchButtonAccumulator::process(const RawEvent* rawEvent) {
    if (rawEvent->type == EV_KEY) {
        switch (rawEvent->code) {
        case BTN_TOUCH:
            mBtnTouch = rawEvent->value;
            break;
        case BTN_STYLUS:
            mBtnStylus = rawEvent->value;
            break;
        case BTN_STYLUS2:
            mBtnStylus2 = rawEvent->value;
            break;
        case BTN_TOOL_FINGER:
            mBtnToolFinger = rawEvent->value;
            break;
        case BTN_TOOL_PEN:
            mBtnToolPen = rawEvent->value;
            break;
        case BTN_TOOL_RUBBER:
            mBtnToolRubber = rawEvent->value;
            break;
        case BTN_TOOL_BRUSH:
            mBtnToolBrush = rawEvent->value;
            break;
        case BTN_TOOL_PENCIL:
            mBtnToolPencil = rawEvent->value;
            break;
        case BTN_TOOL_AIRBRUSH:
            mBtnToolAirbrush = rawEvent->value;
            break;
        case BTN_TOOL_MOUSE:
            mBtnToolMouse = rawEvent->value;
            break;
        case BTN_TOOL_LENS:
            mBtnToolLens = rawEvent->value;
            break;
        case BTN_TOOL_DOUBLETAP:
            mBtnToolDoubleTap = rawEvent->value;
            break;
        case BTN_TOOL_TRIPLETAP:
            mBtnToolTripleTap = rawEvent->value;
            break;
        case BTN_TOOL_QUADTAP:
            mBtnToolQuadTap = rawEvent->value;
            break;
        }
    }
#ifdef LEGACY_TOUCHSCREEN
    // set true to mBtnTouch by multi-touch event with pressure more than zero
    // some touchscreen driver which has BTN_TOUCH feature doesn't send BTN_TOUCH event
    else if (rawEvent->type == EV_ABS && rawEvent->code == ABS_MT_TOUCH_MAJOR && rawEvent->value > 0)
        mBtnTouch = true;
#endif
}

uint32_t TouchButtonAccumulator::getButtonState() const {
    uint32_t result = 0;
    if (mBtnStylus) {
        result |= AMOTION_EVENT_BUTTON_SECONDARY;
    }
    if (mBtnStylus2) {
        result |= AMOTION_EVENT_BUTTON_TERTIARY;
    }
    return result;
}

int32_t TouchButtonAccumulator::getToolType() const {
    if (mBtnToolMouse || mBtnToolLens) {
        return AMOTION_EVENT_TOOL_TYPE_MOUSE;
    }
    if (mBtnToolRubber) {
        return AMOTION_EVENT_TOOL_TYPE_ERASER;
    }
    if (mBtnToolPen || mBtnToolBrush || mBtnToolPencil || mBtnToolAirbrush) {
        return AMOTION_EVENT_TOOL_TYPE_STYLUS;
    }
    if (mBtnToolFinger || mBtnToolDoubleTap || mBtnToolTripleTap || mBtnToolQuadTap) {
        return AMOTION_EVENT_TOOL_TYPE_FINGER;
    }
    return AMOTION_EVENT_TOOL_TYPE_UNKNOWN;
}

bool TouchButtonAccumulator::isToolActive() const {
    return mBtnTouch || mBtnToolFinger || mBtnToolPen || mBtnToolRubber
            || mBtnToolBrush || mBtnToolPencil || mBtnToolAirbrush
            || mBtnToolMouse || mBtnToolLens
            || mBtnToolDoubleTap || mBtnToolTripleTap || mBtnToolQuadTap;
}

bool TouchButtonAccumulator::isHovering() const {
    return mHaveBtnTouch && !mBtnTouch;
}

bool TouchButtonAccumulator::hasStylus() const {
    return mHaveStylus;
}


// --- RawPointerAxes ---

RawPointerAxes::RawPointerAxes() {
    clear();
}

void RawPointerAxes::clear() {
    x.clear();
    y.clear();
    pressure.clear();
    touchMajor.clear();
    touchMinor.clear();
    toolMajor.clear();
    toolMinor.clear();
    orientation.clear();
    distance.clear();
    tiltX.clear();
    tiltY.clear();
    trackingId.clear();
    slot.clear();
}


// --- RawPointerData ---

RawPointerData::RawPointerData() {
    clear();
}

void RawPointerData::clear() {
    pointerCount = 0;
    clearIdBits();
}

void RawPointerData::copyFrom(const RawPointerData& other) {
    pointerCount = other.pointerCount;
    hoveringIdBits = other.hoveringIdBits;
    touchingIdBits = other.touchingIdBits;

    for (uint32_t i = 0; i < pointerCount; i++) {
        pointers[i] = other.pointers[i];

        int id = pointers[i].id;
        idToIndex[id] = other.idToIndex[id];
    }
}

void RawPointerData::getCentroidOfTouchingPointers(float* outX, float* outY) const {
    float x = 0, y = 0;
    uint32_t count = touchingIdBits.count();
    if (count) {
        for (BitSet32 idBits(touchingIdBits); !idBits.isEmpty(); ) {
            uint32_t id = idBits.clearFirstMarkedBit();
            const Pointer& pointer = pointerForId(id);
            x += pointer.x;
            y += pointer.y;
        }
        x /= count;
        y /= count;
    }
    *outX = x;
    *outY = y;
}


// --- CookedPointerData ---

CookedPointerData::CookedPointerData() {
    clear();
}

void CookedPointerData::clear() {
    pointerCount = 0;
    hoveringIdBits.clear();
    touchingIdBits.clear();
}

void CookedPointerData::copyFrom(const CookedPointerData& other) {
    pointerCount = other.pointerCount;
    hoveringIdBits = other.hoveringIdBits;
    touchingIdBits = other.touchingIdBits;

    for (uint32_t i = 0; i < pointerCount; i++) {
        pointerProperties[i].copyFrom(other.pointerProperties[i]);
        pointerCoords[i].copyFrom(other.pointerCoords[i]);

        int id = pointerProperties[i].id;
        idToIndex[id] = other.idToIndex[id];
    }
}


// --- SingleTouchMotionAccumulator ---

SingleTouchMotionAccumulator::SingleTouchMotionAccumulator() {
    clearAbsoluteAxes();
}

void SingleTouchMotionAccumulator::reset(InputDevice* device) {
    mAbsX = device->getAbsoluteAxisValue(ABS_X);
    mAbsY = device->getAbsoluteAxisValue(ABS_Y);
    mAbsPressure = device->getAbsoluteAxisValue(ABS_PRESSURE);
    mAbsToolWidth = device->getAbsoluteAxisValue(ABS_TOOL_WIDTH);
    mAbsDistance = device->getAbsoluteAxisValue(ABS_DISTANCE);
    mAbsTiltX = device->getAbsoluteAxisValue(ABS_TILT_X);
    mAbsTiltY = device->getAbsoluteAxisValue(ABS_TILT_Y);
}

void SingleTouchMotionAccumulator::clearAbsoluteAxes() {
    mAbsX = 0;
    mAbsY = 0;
    mAbsPressure = 0;
    mAbsToolWidth = 0;
    mAbsDistance = 0;
    mAbsTiltX = 0;
    mAbsTiltY = 0;
}

void SingleTouchMotionAccumulator::process(const RawEvent* rawEvent) {
    if (rawEvent->type == EV_ABS) {
        switch (rawEvent->code) {
        case ABS_X:
            mAbsX = rawEvent->value;
            break;
        case ABS_Y:
            mAbsY = rawEvent->value;
            break;
        case ABS_PRESSURE:
            mAbsPressure = rawEvent->value;
            break;
        case ABS_TOOL_WIDTH:
            mAbsToolWidth = rawEvent->value;
            break;
        case ABS_DISTANCE:
            mAbsDistance = rawEvent->value;
            break;
        case ABS_TILT_X:
            mAbsTiltX = rawEvent->value;
            break;
        case ABS_TILT_Y:
            mAbsTiltY = rawEvent->value;
            break;
        }
    }
}


// --- MultiTouchMotionAccumulator ---

MultiTouchMotionAccumulator::MultiTouchMotionAccumulator() :
        mCurrentSlot(-1), mSlots(NULL), mSlotCount(0), mUsingSlotsProtocol(false),
        mHaveStylus(false) {
}

MultiTouchMotionAccumulator::~MultiTouchMotionAccumulator() {
    delete[] mSlots;
}

void MultiTouchMotionAccumulator::configure(InputDevice* device,
        size_t slotCount, bool usingSlotsProtocol) {
    mSlotCount = slotCount;
    mUsingSlotsProtocol = usingSlotsProtocol;
    mHaveStylus = device->hasAbsoluteAxis(ABS_MT_TOOL_TYPE);

    delete[] mSlots;
    mSlots = new Slot[slotCount];
}

void MultiTouchMotionAccumulator::reset(InputDevice* device) {
    // Unfortunately there is no way to read the initial contents of the slots.
    // So when we reset the accumulator, we must assume they are all zeroes.
    if (mUsingSlotsProtocol) {
        // Query the driver for the current slot index and use it as the initial slot
        // before we start reading events from the device.  It is possible that the
        // current slot index will not be the same as it was when the first event was
        // written into the evdev buffer, which means the input mapper could start
        // out of sync with the initial state of the events in the evdev buffer.
        // In the extremely unlikely case that this happens, the data from
        // two slots will be confused until the next ABS_MT_SLOT event is received.
        // This can cause the touch point to "jump", but at least there will be
        // no stuck touches.
        int32_t initialSlot;
        status_t status = device->getEventHub()->getAbsoluteAxisValue(device->getId(),
                ABS_MT_SLOT, &initialSlot);
        if (status) {
            ALOGD("Could not retrieve current multitouch slot index.  status=%d", status);
            initialSlot = -1;
        }
        clearSlots(initialSlot);
    } else {
        clearSlots(-1);
    }
}

void MultiTouchMotionAccumulator::clearSlots(int32_t initialSlot) {
    if (mSlots) {
        for (size_t i = 0; i < mSlotCount; i++) {
            mSlots[i].clear();
        }
    }
    mCurrentSlot = initialSlot;
}

void MultiTouchMotionAccumulator::process(const RawEvent* rawEvent) {
    if (rawEvent->type == EV_ABS) {
        bool newSlot = false;
        if (mUsingSlotsProtocol) {
            if (rawEvent->code == ABS_MT_SLOT) {
                mCurrentSlot = rawEvent->value;
                newSlot = true;
            }
        } else if (mCurrentSlot < 0) {
            mCurrentSlot = 0;
        }

        if (mCurrentSlot < 0 || size_t(mCurrentSlot) >= mSlotCount) {
#if DEBUG_POINTERS
            if (newSlot) {
                ALOGW("MultiTouch device emitted invalid slot index %d but it "
                        "should be between 0 and %d; ignoring this slot.",
                        mCurrentSlot, mSlotCount - 1);
            }
#endif
        } else {
            Slot* slot = &mSlots[mCurrentSlot];

            switch (rawEvent->code) {
            case ABS_MT_POSITION_X:
                slot->mInUse = true;
                slot->mAbsMTPositionX = rawEvent->value;
                break;
            case ABS_MT_POSITION_Y:
                slot->mInUse = true;
                slot->mAbsMTPositionY = rawEvent->value;
                break;
            case ABS_MT_TOUCH_MAJOR:
                slot->mInUse = true;
#ifdef LEGACY_TOUCHSCREEN
                // emulate ABS_MT_PRESSURE
                slot->mAbsMTPressure = rawEvent->value;
#else
                slot->mAbsMTTouchMajor = rawEvent->value;
#endif
                break;
            case ABS_MT_TOUCH_MINOR:
                slot->mInUse = true;
                slot->mAbsMTTouchMinor = rawEvent->value;
                slot->mHaveAbsMTTouchMinor = true;
                break;
            case ABS_MT_WIDTH_MAJOR:
                slot->mInUse = true;
#ifdef LEGACY_TOUCHSCREEN
                // emulate ABS_MT_TOUCH_MAJOR
                slot->mAbsMTTouchMajor = rawEvent->value;
#else
                slot->mAbsMTWidthMajor = rawEvent->value;
#endif
                break;
            case ABS_MT_WIDTH_MINOR:
                slot->mInUse = true;
                slot->mAbsMTWidthMinor = rawEvent->value;
                slot->mHaveAbsMTWidthMinor = true;
                break;
            case ABS_MT_ORIENTATION:
                slot->mInUse = true;
                slot->mAbsMTOrientation = rawEvent->value;
                break;
            case ABS_MT_TRACKING_ID:
                if (mUsingSlotsProtocol && rawEvent->value < 0) {
                    // The slot is no longer in use but it retains its previous contents,
                    // which may be reused for subsequent touches.
                    slot->mInUse = false;
                } else {
                    slot->mInUse = true;
                    slot->mAbsMTTrackingId = rawEvent->value;
                }
                break;
            case ABS_MT_PRESSURE:
                slot->mInUse = true;
                slot->mAbsMTPressure = rawEvent->value;
                break;
            case ABS_MT_DISTANCE:
                slot->mInUse = true;
                slot->mAbsMTDistance = rawEvent->value;
                break;
            case ABS_MT_TOOL_TYPE:
                slot->mInUse = true;
                slot->mAbsMTToolType = rawEvent->value;
                slot->mHaveAbsMTToolType = true;
                break;
            }
        }
    } else if (rawEvent->type == EV_SYN && rawEvent->code == SYN_MT_REPORT) {
#ifdef LEGACY_TOUCHSCREEN
        // don't use the slot with pressure less than or qeual to zero
        // some touchscreen driver sends multi-touch event for not-in-use pointer
        if (mSlots[mCurrentSlot].mAbsMTPressure <= 0)
            mSlots[mCurrentSlot].mInUse = false;
#endif
        // MultiTouch Sync: The driver has returned all data for *one* of the pointers.
        mCurrentSlot += 1;
    }
}

void MultiTouchMotionAccumulator::finishSync() {
    if (!mUsingSlotsProtocol) {
        clearSlots(-1);
    }
}

bool MultiTouchMotionAccumulator::hasStylus() const {
    return mHaveStylus;
}


// --- MultiTouchMotionAccumulator::Slot ---

MultiTouchMotionAccumulator::Slot::Slot() {
    clear();
}

void MultiTouchMotionAccumulator::Slot::clear() {
    mInUse = false;
    mHaveAbsMTTouchMinor = false;
    mHaveAbsMTWidthMinor = false;
    mHaveAbsMTToolType = false;
    mAbsMTPositionX = 0;
    mAbsMTPositionY = 0;
    mAbsMTTouchMajor = 0;
    mAbsMTTouchMinor = 0;
    mAbsMTWidthMajor = 0;
    mAbsMTWidthMinor = 0;
    mAbsMTOrientation = 0;
    mAbsMTTrackingId = -1;
    mAbsMTPressure = 0;
    mAbsMTDistance = 0;
    mAbsMTToolType = 0;
}

int32_t MultiTouchMotionAccumulator::Slot::getToolType() const {
    if (mHaveAbsMTToolType) {
        switch (mAbsMTToolType) {
        case MT_TOOL_FINGER:
            return AMOTION_EVENT_TOOL_TYPE_FINGER;
        case MT_TOOL_PEN:
            return AMOTION_EVENT_TOOL_TYPE_STYLUS;
        }
    }
    return AMOTION_EVENT_TOOL_TYPE_UNKNOWN;
}


// --- InputMapper ---

InputMapper::InputMapper(InputDevice* device) :
        mDevice(device), mContext(device->getContext()) {
}

InputMapper::~InputMapper() {
}

void InputMapper::populateDeviceInfo(InputDeviceInfo* info) {
    info->addSource(getSources());
}

void InputMapper::dump(String8& dump) {
}

void InputMapper::configure(nsecs_t when,
        const InputReaderConfiguration* config, uint32_t changes) {
}

void InputMapper::reset(nsecs_t when) {
}

void InputMapper::timeoutExpired(nsecs_t when) {
}

int32_t InputMapper::getKeyCodeState(uint32_t sourceMask, int32_t keyCode) {
    return AKEY_STATE_UNKNOWN;
}

int32_t InputMapper::getScanCodeState(uint32_t sourceMask, int32_t scanCode) {
    return AKEY_STATE_UNKNOWN;
}

int32_t InputMapper::getSwitchState(uint32_t sourceMask, int32_t switchCode) {
    return AKEY_STATE_UNKNOWN;
}

bool InputMapper::markSupportedKeyCodes(uint32_t sourceMask, size_t numCodes,
        const int32_t* keyCodes, uint8_t* outFlags) {
    return false;
}

void InputMapper::vibrate(const nsecs_t* pattern, size_t patternSize, ssize_t repeat,
        int32_t token) {
}

void InputMapper::cancelVibrate(int32_t token) {
}

int32_t InputMapper::getMetaState() {
    return 0;
}

void InputMapper::fadePointer() {
}

status_t InputMapper::getAbsoluteAxisInfo(int32_t axis, RawAbsoluteAxisInfo* axisInfo) {
    return getEventHub()->getAbsoluteAxisInfo(getDeviceId(), axis, axisInfo);
}

void InputMapper::bumpGeneration() {
    mDevice->bumpGeneration();
}

void InputMapper::dumpRawAbsoluteAxisInfo(String8& dump,
        const RawAbsoluteAxisInfo& axis, const char* name) {
    if (axis.valid) {
        dump.appendFormat(INDENT4 "%s: min=%d, max=%d, flat=%d, fuzz=%d, resolution=%d\n",
                name, axis.minValue, axis.maxValue, axis.flat, axis.fuzz, axis.resolution);
    } else {
        dump.appendFormat(INDENT4 "%s: unknown range\n", name);
    }
}


// --- SwitchInputMapper ---

SwitchInputMapper::SwitchInputMapper(InputDevice* device) :
        InputMapper(device) {
}

SwitchInputMapper::~SwitchInputMapper() {
}

uint32_t SwitchInputMapper::getSources() {
    return AINPUT_SOURCE_SWITCH;
}

void SwitchInputMapper::process(const RawEvent* rawEvent) {
    switch (rawEvent->type) {
    case EV_SW:
        processSwitch(rawEvent->when, rawEvent->code, rawEvent->value);
        break;
    }
}

void SwitchInputMapper::processSwitch(nsecs_t when, int32_t switchCode, int32_t switchValue) {
    NotifySwitchArgs args(when, 0, switchCode, switchValue);
    getListener()->notifySwitch(&args);
}

int32_t SwitchInputMapper::getSwitchState(uint32_t sourceMask, int32_t switchCode) {
    return getEventHub()->getSwitchState(getDeviceId(), switchCode);
}


// --- VibratorInputMapper ---

VibratorInputMapper::VibratorInputMapper(InputDevice* device) :
        InputMapper(device), mVibrating(false) {
}

VibratorInputMapper::~VibratorInputMapper() {
}

uint32_t VibratorInputMapper::getSources() {
    return 0;
}

void VibratorInputMapper::populateDeviceInfo(InputDeviceInfo* info) {
    InputMapper::populateDeviceInfo(info);

    info->setVibrator(true);
}

void VibratorInputMapper::process(const RawEvent* rawEvent) {
    // TODO: Handle FF_STATUS, although it does not seem to be widely supported.
}

void VibratorInputMapper::vibrate(const nsecs_t* pattern, size_t patternSize, ssize_t repeat,
        int32_t token) {
#if DEBUG_VIBRATOR
    String8 patternStr;
    for (size_t i = 0; i < patternSize; i++) {
        if (i != 0) {
            patternStr.append(", ");
        }
        patternStr.appendFormat("%lld", pattern[i]);
    }
    ALOGD("vibrate: deviceId=%d, pattern=[%s], repeat=%ld, token=%d",
            getDeviceId(), patternStr.string(), repeat, token);
#endif

    mVibrating = true;
    memcpy(mPattern, pattern, patternSize * sizeof(nsecs_t));
    mPatternSize = patternSize;
    mRepeat = repeat;
    mToken = token;
    mIndex = -1;

    nextStep();
}

void VibratorInputMapper::cancelVibrate(int32_t token) {
#if DEBUG_VIBRATOR
    ALOGD("cancelVibrate: deviceId=%d, token=%d", getDeviceId(), token);
#endif

    if (mVibrating && mToken == token) {
        stopVibrating();
    }
}

void VibratorInputMapper::timeoutExpired(nsecs_t when) {
    if (mVibrating) {
        if (when >= mNextStepTime) {
            nextStep();
        } else {
            getContext()->requestTimeoutAtTime(mNextStepTime);
        }
    }
}

void VibratorInputMapper::nextStep() {
    mIndex += 1;
    if (size_t(mIndex) >= mPatternSize) {
        if (mRepeat < 0) {
            // We are done.
            stopVibrating();
            return;
        }
        mIndex = mRepeat;
    }

    bool vibratorOn = mIndex & 1;
    nsecs_t duration = mPattern[mIndex];
    if (vibratorOn) {
#if DEBUG_VIBRATOR
        ALOGD("nextStep: sending vibrate deviceId=%d, duration=%lld",
                getDeviceId(), duration);
#endif
        getEventHub()->vibrate(getDeviceId(), duration);
    } else {
#if DEBUG_VIBRATOR
        ALOGD("nextStep: sending cancel vibrate deviceId=%d", getDeviceId());
#endif
        getEventHub()->cancelVibrate(getDeviceId());
    }
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    mNextStepTime = now + duration;
    getContext()->requestTimeoutAtTime(mNextStepTime);
#if DEBUG_VIBRATOR
    ALOGD("nextStep: scheduled timeout in %0.3fms", duration * 0.000001f);
#endif
}

void VibratorInputMapper::stopVibrating() {
    mVibrating = false;
#if DEBUG_VIBRATOR
    ALOGD("stopVibrating: sending cancel vibrate deviceId=%d", getDeviceId());
#endif
    getEventHub()->cancelVibrate(getDeviceId());
}

void VibratorInputMapper::dump(String8& dump) {
    dump.append(INDENT2 "Vibrator Input Mapper:\n");
    dump.appendFormat(INDENT3 "Vibrating: %s\n", toString(mVibrating));
}


// --- KeyboardInputMapper ---

KeyboardInputMapper::KeyboardInputMapper(InputDevice* device,
        uint32_t source, int32_t keyboardType) :
        InputMapper(device), mSource(source),
        mKeyboardType(keyboardType) {
}

KeyboardInputMapper::~KeyboardInputMapper() {
}

uint32_t KeyboardInputMapper::getSources() {
    return mSource;
}

void KeyboardInputMapper::populateDeviceInfo(InputDeviceInfo* info) {
    InputMapper::populateDeviceInfo(info);

    info->setKeyboardType(mKeyboardType);
    info->setKeyCharacterMap(getEventHub()->getKeyCharacterMap(getDeviceId()));
}

void KeyboardInputMapper::dump(String8& dump) {
    dump.append(INDENT2 "Keyboard Input Mapper:\n");
    dumpParameters(dump);
    dump.appendFormat(INDENT3 "KeyboardType: %d\n", mKeyboardType);
    dump.appendFormat(INDENT3 "Orientation: %d\n", mOrientation);
    dump.appendFormat(INDENT3 "KeyDowns: %d keys currently down\n", mKeyDowns.size());
    dump.appendFormat(INDENT3 "MetaState: 0x%0x\n", mMetaState);
    dump.appendFormat(INDENT3 "DownTime: %lld\n", mDownTime);
}


void KeyboardInputMapper::configure(nsecs_t when,
        const InputReaderConfiguration* config, uint32_t changes) {
    InputMapper::configure(when, config, changes);

    if (!changes) { // first time only
        // Configure basic parameters.
        configureParameters();
    }

    if (!changes || (changes & InputReaderConfiguration::CHANGE_DISPLAY_INFO)) {
        if (mParameters.orientationAware && mParameters.associatedDisplayId >= 0) {
            if (!config->getDisplayInfo(mParameters.associatedDisplayId,
                        false /*external*/, NULL, NULL, &mOrientation)) {
                mOrientation = DISPLAY_ORIENTATION_0;
            }
        } else {
            mOrientation = DISPLAY_ORIENTATION_0;
        }
    }
}

void KeyboardInputMapper::configureParameters() {
    mParameters.orientationAware = false;
    getDevice()->getConfiguration().tryGetProperty(String8("keyboard.orientationAware"),
            mParameters.orientationAware);

    mParameters.associatedDisplayId = -1;
    if (mParameters.orientationAware) {
        mParameters.associatedDisplayId = 0;
    }
}

void KeyboardInputMapper::dumpParameters(String8& dump) {
    dump.append(INDENT3 "Parameters:\n");
    dump.appendFormat(INDENT4 "AssociatedDisplayId: %d\n",
            mParameters.associatedDisplayId);
    dump.appendFormat(INDENT4 "OrientationAware: %s\n",
            toString(mParameters.orientationAware));
}

void KeyboardInputMapper::reset(nsecs_t when) {
    mMetaState = AMETA_NONE;
    mDownTime = 0;
    mKeyDowns.clear();
    mCurrentHidUsage = 0;

    resetLedState();

    InputMapper::reset(when);
}

void KeyboardInputMapper::process(const RawEvent* rawEvent) {
    switch (rawEvent->type) {
    case EV_KEY: {
        int32_t scanCode = rawEvent->code;
        int32_t usageCode = mCurrentHidUsage;
        mCurrentHidUsage = 0;

        if (isKeyboardOrGamepadKey(scanCode)) {
            int32_t keyCode;
            uint32_t flags;
            if (getEventHub()->mapKey(getDeviceId(), scanCode, usageCode, &keyCode, &flags)) {
                keyCode = AKEYCODE_UNKNOWN;
                flags = 0;
            }
            // c8690 key input patch
            //by kaasnake
            // prevent of interpretting REPEAT (rawEvent->value == 2) event as DOWN (rawEvent->value == 1)
            if (rawEvent->value == 2)
	      break;
	    // /c8690 key input patch
            processKey(rawEvent->when, rawEvent->value != 0, keyCode, scanCode, flags);
        }
        break;
    }
    case EV_MSC: {
        if (rawEvent->code == MSC_SCAN) {
            mCurrentHidUsage = rawEvent->value;
        }
        break;
    }
    case EV_SYN: {
        if (rawEvent->code == SYN_REPORT) {
            mCurrentHidUsage = 0;
        }
    }
    }
}

bool KeyboardInputMapper::isKeyboardOrGamepadKey(int32_t scanCode) {
    return scanCode < BTN_MOUSE
        || scanCode >= KEY_OK
        || (scanCode >= BTN_MISC && scanCode < BTN_MOUSE)
        || (scanCode >= BTN_JOYSTICK && scanCode < BTN_DIGI);
}

void KeyboardInputMapper::processKey(nsecs_t when, bool down, int32_t keyCode,
        int32_t scanCode, uint32_t policyFlags) {

    if (down) {
        // Rotate key codes according to orientation if needed.
        if (mParameters.orientationAware && mParameters.associatedDisplayId >= 0) {
            keyCode = rotateKeyCode(keyCode, mOrientation);
        }

        // Add key down.
        ssize_t keyDownIndex = findKeyDown(scanCode);
        if (keyDownIndex >= 0) {
            // key repeat, be sure to use same keycode as before in case of rotation
            keyCode = mKeyDowns.itemAt(keyDownIndex).keyCode;
        } else {
            // key down
            if ((policyFlags & POLICY_FLAG_VIRTUAL)
                    && mContext->shouldDropVirtualKey(when,
                            getDevice(), keyCode, scanCode)) {
                return;
            }

            mKeyDowns.push();
            KeyDown& keyDown = mKeyDowns.editTop();
            keyDown.keyCode = keyCode;
            keyDown.scanCode = scanCode;
        }

        mDownTime = when;
    } else {
        // Remove key down.
        ssize_t keyDownIndex = findKeyDown(scanCode);
        if (keyDownIndex >= 0) {
            // key up, be sure to use same keycode as before in case of rotation
            keyCode = mKeyDowns.itemAt(keyDownIndex).keyCode;
            mKeyDowns.removeAt(size_t(keyDownIndex));
        } else {
            // key was not actually down
            ALOGI("Dropping key up from device %s because the key was not down.  "
                    "keyCode=%d, scanCode=%d",
                    getDeviceName().string(), keyCode, scanCode);
            return;
        }
    }

    bool metaStateChanged = false;
    int32_t oldMetaState = mMetaState;
    int32_t newMetaState = updateMetaState(keyCode, down, oldMetaState);
    if (oldMetaState != newMetaState) {
        mMetaState = newMetaState;
        metaStateChanged = true;
        updateLedState(false);
    }

    nsecs_t downTime = mDownTime;

    // Key down on external an keyboard should wake the device.
    // We don't do this for internal keyboards to prevent them from waking up in your pocket.
    // For internal keyboards, the key layout file should specify the policy flags for
    // each wake key individually.
    // TODO: Use the input device configuration to control this behavior more finely.
    if (down && getDevice()->isExternal()
            && !(policyFlags & (POLICY_FLAG_WAKE | POLICY_FLAG_WAKE_DROPPED))) {
        policyFlags |= POLICY_FLAG_WAKE_DROPPED;
    }

    if (metaStateChanged) {
        getContext()->updateGlobalMetaState();
    }

    if (down && !isMetaKey(keyCode)) {
        getContext()->fadePointer();
    }

    NotifyKeyArgs args(when, getDeviceId(), mSource, policyFlags,
            down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP,
            AKEY_EVENT_FLAG_FROM_SYSTEM, keyCode, scanCode, newMetaState, downTime);
    getListener()->notifyKey(&args);
}

ssize_t KeyboardInputMapper::findKeyDown(int32_t scanCode) {
    size_t n = mKeyDowns.size();
    for (size_t i = 0; i < n; i++) {
        if (mKeyDowns[i].scanCode == scanCode) {
            return i;
        }
    }
    return -1;
}

int32_t KeyboardInputMapper::getKeyCodeState(uint32_t sourceMask, int32_t keyCode) {
    return getEventHub()->getKeyCodeState(getDeviceId(), keyCode);
}

int32_t KeyboardInputMapper::getScanCodeState(uint32_t sourceMask, int32_t scanCode) {
    return getEventHub()->getScanCodeState(getDeviceId(), scanCode);
}

bool KeyboardInputMapper::markSupportedKeyCodes(uint32_t sourceMask, size_t numCodes,
        const int32_t* keyCodes, uint8_t* outFlags) {
    return getEventHub()->markSupportedKeyCodes(getDeviceId(), numCodes, keyCodes, outFlags);
}

int32_t KeyboardInputMapper::getMetaState() {
    return mMetaState;
}

void KeyboardInputMapper::resetLedState() {
    initializeLedState(mCapsLockLedState, LED_CAPSL);
    initializeLedState(mNumLockLedState, LED_NUML);
    initializeLedState(mScrollLockLedState, LED_SCROLLL);

    updateLedState(true);
}

void KeyboardInputMapper::initializeLedState(LedState& ledState, int32_t led) {
    ledState.avail = getEventHub()->hasLed(getDeviceId(), led);
    ledState.on = false;
}

void KeyboardInputMapper::updateLedState(bool reset) {
    updateLedStateForModifier(mCapsLockLedState, LED_CAPSL,
            AMETA_CAPS_LOCK_ON, reset);
    updateLedStateForModifier(mNumLockLedState, LED_NUML,
            AMETA_NUM_LOCK_ON, reset);
    updateLedStateForModifier(mScrollLockLedState, LED_SCROLLL,
            AMETA_SCROLL_LOCK_ON, reset);
}

void KeyboardInputMapper::updateLedStateForModifier(LedState& ledState,
        int32_t led, int32_t modifier, bool reset) {
    if (ledState.avail) {
        bool desiredState = (mMetaState & modifier) != 0;
        if (reset || ledState.on != desiredState) {
            getEventHub()->setLedState(getDeviceId(), led, desiredState);
            ledState.on = desiredState;
        }
    }
}


// --- CursorInputMapper ---

CursorInputMapper::CursorInputMapper(InputDevice* device) :
        InputMapper(device) {
}

CursorInputMapper::~CursorInputMapper() {
}

uint32_t CursorInputMapper::getSources() {
    return mSource;
}

void CursorInputMapper::populateDeviceInfo(InputDeviceInfo* info) {
    InputMapper::populateDeviceInfo(info);

    if (mParameters.mode == Parameters::MODE_POINTER) {
        float minX, minY, maxX, maxY;
        if (mPointerController->getBounds(&minX, &minY, &maxX, &maxY)) {
            info->addMotionRange(AMOTION_EVENT_AXIS_X, mSource, minX, maxX, 0.0f, 0.0f);
            info->addMotionRange(AMOTION_EVENT_AXIS_Y, mSource, minY, maxY, 0.0f, 0.0f);
        }
    } else {
        info->addMotionRange(AMOTION_EVENT_AXIS_X, mSource, -1.0f, 1.0f, 0.0f, mXScale);
        info->addMotionRange(AMOTION_EVENT_AXIS_Y, mSource, -1.0f, 1.0f, 0.0f, mYScale);
    }
    info->addMotionRange(AMOTION_EVENT_AXIS_PRESSURE, mSource, 0.0f, 1.0f, 0.0f, 0.0f);

    if (mCursorScrollAccumulator.haveRelativeVWheel()) {
        info->addMotionRange(AMOTION_EVENT_AXIS_VSCROLL, mSource, -1.0f, 1.0f, 0.0f, 0.0f);
    }
    if (mCursorScrollAccumulator.haveRelativeHWheel()) {
        info->addMotionRange(AMOTION_EVENT_AXIS_HSCROLL, mSource, -1.0f, 1.0f, 0.0f, 0.0f);
    }
}

void CursorInputMapper::dump(String8& dump) {
    dump.append(INDENT2 "Cursor Input Mapper:\n");
    dumpParameters(dump);
    dump.appendFormat(INDENT3 "XScale: %0.3f\n", mXScale);
    dump.appendFormat(INDENT3 "YScale: %0.3f\n", mYScale);
    dump.appendFormat(INDENT3 "XPrecision: %0.3f\n", mXPrecision);
    dump.appendFormat(INDENT3 "YPrecision: %0.3f\n", mYPrecision);
    dump.appendFormat(INDENT3 "HaveVWheel: %s\n",
            toString(mCursorScrollAccumulator.haveRelativeVWheel()));
    dump.appendFormat(INDENT3 "HaveHWheel: %s\n",
            toString(mCursorScrollAccumulator.haveRelativeHWheel()));
    dump.appendFormat(INDENT3 "VWheelScale: %0.3f\n", mVWheelScale);
    dump.appendFormat(INDENT3 "HWheelScale: %0.3f\n", mHWheelScale);
    dump.appendFormat(INDENT3 "Orientation: %d\n", mOrientation);
    dump.appendFormat(INDENT3 "ButtonState: 0x%08x\n", mButtonState);
    dump.appendFormat(INDENT3 "Down: %s\n", toString(isPointerDown(mButtonState)));
    dump.appendFormat(INDENT3 "DownTime: %lld\n", mDownTime);
}

void CursorInputMapper::configure(nsecs_t when,
        const InputReaderConfiguration* config, uint32_t changes) {
    InputMapper::configure(when, config, changes);

    if (!changes) { // first time only
        mCursorScrollAccumulator.configure(getDevice());

        // Configure basic parameters.
        configureParameters();

        // Configure device mode.
        switch (mParameters.mode) {
        case Parameters::MODE_POINTER:
            mSource = AINPUT_SOURCE_MOUSE;
            mXPrecision = 1.0f;
            mYPrecision = 1.0f;
            mXScale = 1.0f;
            mYScale = 1.0f;
            mPointerController = getPolicy()->obtainPointerController(getDeviceId());
            break;
        case Parameters::MODE_NAVIGATION:
            mSource = AINPUT_SOURCE_TRACKBALL;
            mXPrecision = TRACKBALL_MOVEMENT_THRESHOLD;
            mYPrecision = TRACKBALL_MOVEMENT_THRESHOLD;
            mXScale = 1.0f / TRACKBALL_MOVEMENT_THRESHOLD;
            mYScale = 1.0f / TRACKBALL_MOVEMENT_THRESHOLD;
            break;
        }

        mVWheelScale = 1.0f;
        mHWheelScale = 1.0f;
    }

    if (!changes || (changes & InputReaderConfiguration::CHANGE_POINTER_SPEED)) {
        mPointerVelocityControl.setParameters(config->pointerVelocityControlParameters);
        mWheelXVelocityControl.setParameters(config->wheelVelocityControlParameters);
        mWheelYVelocityControl.setParameters(config->wheelVelocityControlParameters);
    }

    if (!changes || (changes & InputReaderConfiguration::CHANGE_DISPLAY_INFO)) {
        if (mParameters.orientationAware && mParameters.associatedDisplayId >= 0) {
            if (!config->getDisplayInfo(mParameters.associatedDisplayId,
                        false /*external*/, NULL, NULL, &mOrientation)) {
                mOrientation = DISPLAY_ORIENTATION_0;
            }
        } else {
            mOrientation = DISPLAY_ORIENTATION_0;
        }
        bumpGeneration();
    }
}

void CursorInputMapper::configureParameters() {
    mParameters.mode = Parameters::MODE_POINTER;
    String8 cursorModeString;
    if (getDevice()->getConfiguration().tryGetProperty(String8("cursor.mode"), cursorModeString)) {
        if (cursorModeString == "navigation") {
            mParameters.mode = Parameters::MODE_NAVIGATION;
        } else if (cursorModeString != "pointer" && cursorModeString != "default") {
            ALOGW("Invalid value for cursor.mode: '%s'", cursorModeString.string());
        }
    }

    mParameters.orientationAware = false;
    getDevice()->getConfiguration().tryGetProperty(String8("cursor.orientationAware"),
            mParameters.orientationAware);

    mParameters.associatedDisplayId = -1;
    if (mParameters.mode == Parameters::MODE_POINTER || mParameters.orientationAware) {
        mParameters.associatedDisplayId = 0;
    }
}

void CursorInputMapper::dumpParameters(String8& dump) {
    dump.append(INDENT3 "Parameters:\n");
    dump.appendFormat(INDENT4 "AssociatedDisplayId: %d\n",
            mParameters.associatedDisplayId);

    switch (mParameters.mode) {
    case Parameters::MODE_POINTER:
        dump.append(INDENT4 "Mode: pointer\n");
        break;
    case Parameters::MODE_NAVIGATION:
        dump.append(INDENT4 "Mode: navigation\n");
        break;
    default:
        ALOG_ASSERT(false);
    }

    dump.appendFormat(INDENT4 "OrientationAware: %s\n",
            toString(mParameters.orientationAware));
}

void CursorInputMapper::reset(nsecs_t when) {
    mButtonState = 0;
    mDownTime = 0;

    mPointerVelocityControl.reset();
    mWheelXVelocityControl.reset();
    mWheelYVelocityControl.reset();

    mCursorButtonAccumulator.reset(getDevice());
    mCursorMotionAccumulator.reset(getDevice());
    mCursorScrollAccumulator.reset(getDevice());

    InputMapper::reset(when);
}

void CursorInputMapper::process(const RawEvent* rawEvent) {
    mCursorButtonAccumulator.process(rawEvent);
    mCursorMotionAccumulator.process(rawEvent);
    mCursorScrollAccumulator.process(rawEvent);

    if (rawEvent->type == EV_SYN && rawEvent->code == SYN_REPORT) {
        sync(rawEvent->when);
    }
#ifdef LEGACY_TRACKPAD
    // sync now since BTN_MOUSE is not necessarily followed by SYN_REPORT and
    // we need to ensure that we report the up/down promptly.
    else if (rawEvent->type == EV_KEY && rawEvent->code == BTN_MOUSE) {
        sync(rawEvent->when);
    }
#endif
}

void CursorInputMapper::sync(nsecs_t when) {
    int32_t lastButtonState = mButtonState;
    int32_t currentButtonState = mCursorButtonAccumulator.getButtonState();
    mButtonState = currentButtonState;

    bool wasDown = isPointerDown(lastButtonState);
    bool down = isPointerDown(currentButtonState);
    bool downChanged;
    if (!wasDown && down) {
        mDownTime = when;
        downChanged = true;
    } else if (wasDown && !down) {
        downChanged = true;
    } else {
        downChanged = false;
    }
    nsecs_t downTime = mDownTime;
    bool buttonsChanged = currentButtonState != lastButtonState;
    bool buttonsPressed = currentButtonState & ~lastButtonState;

    float deltaX = mCursorMotionAccumulator.getRelativeX() * mXScale;
    float deltaY = mCursorMotionAccumulator.getRelativeY() * mYScale;
    bool moved = deltaX != 0 || deltaY != 0;

    // Rotate delta according to orientation if needed.
    if (mParameters.orientationAware && mParameters.associatedDisplayId >= 0
            && (deltaX != 0.0f || deltaY != 0.0f)) {
        rotateDelta(mOrientation, &deltaX, &deltaY);
    }

    // Move the pointer.
    PointerProperties pointerProperties;
    pointerProperties.clear();
    pointerProperties.id = 0;
    pointerProperties.toolType = AMOTION_EVENT_TOOL_TYPE_MOUSE;

    PointerCoords pointerCoords;
    pointerCoords.clear();

    float vscroll = mCursorScrollAccumulator.getRelativeVWheel();
    float hscroll = mCursorScrollAccumulator.getRelativeHWheel();
    bool scrolled = vscroll != 0 || hscroll != 0;

    mWheelYVelocityControl.move(when, NULL, &vscroll);
    mWheelXVelocityControl.move(when, &hscroll, NULL);

    mPointerVelocityControl.move(when, &deltaX, &deltaY);

    if (mPointerController != NULL) {
        if (moved || scrolled || buttonsChanged) {
            mPointerController->setPresentation(
                    PointerControllerInterface::PRESENTATION_POINTER);

            if (moved) {
                mPointerController->move(deltaX, deltaY);
            }

            if (buttonsChanged) {
                mPointerController->setButtonState(currentButtonState);
            }

            mPointerController->unfade(PointerControllerInterface::TRANSITION_IMMEDIATE);
        }

        float x, y;
        mPointerController->getPosition(&x, &y);
        pointerCoords.setAxisValue(AMOTION_EVENT_AXIS_X, x);
        pointerCoords.setAxisValue(AMOTION_EVENT_AXIS_Y, y);
    } else {
        pointerCoords.setAxisValue(AMOTION_EVENT_AXIS_X, deltaX);
        pointerCoords.setAxisValue(AMOTION_EVENT_AXIS_Y, deltaY);
    }

    pointerCoords.setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, down ? 1.0f : 0.0f);

    // Moving an external trackball or mouse should wake the device.
    // We don't do this for internal cursor devices to prevent them from waking up
    // the device in your pocket.
    // TODO: Use the input device configuration to control this behavior more finely.
    uint32_t policyFlags = 0;
    if ((buttonsPressed || moved || scrolled) && getDevice()->isExternal()) {
        policyFlags |= POLICY_FLAG_WAKE_DROPPED;
    }

    // Synthesize key down from buttons if needed.
    synthesizeButtonKeys(getContext(), AKEY_EVENT_ACTION_DOWN, when, getDeviceId(), mSource,
            policyFlags, lastButtonState, currentButtonState);

    // Send motion event.
    if (downChanged || moved || scrolled || buttonsChanged) {
        int32_t metaState = mContext->getGlobalMetaState();
        int32_t motionEventAction;
        if (downChanged) {
            motionEventAction = down ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP;
        } else if (down || mPointerController == NULL) {
            motionEventAction = AMOTION_EVENT_ACTION_MOVE;
        } else {
            motionEventAction = AMOTION_EVENT_ACTION_HOVER_MOVE;
        }

        NotifyMotionArgs args(when, getDeviceId(), mSource, policyFlags,
                motionEventAction, 0, metaState, currentButtonState, 0,
                1, &pointerProperties, &pointerCoords, mXPrecision, mYPrecision, downTime);
        getListener()->notifyMotion(&args);

        // Send hover move after UP to tell the application that the mouse is hovering now.
        if (motionEventAction == AMOTION_EVENT_ACTION_UP
                && mPointerController != NULL) {
            NotifyMotionArgs hoverArgs(when, getDeviceId(), mSource, policyFlags,
                    AMOTION_EVENT_ACTION_HOVER_MOVE, 0,
                    metaState, currentButtonState, AMOTION_EVENT_EDGE_FLAG_NONE,
                    1, &pointerProperties, &pointerCoords, mXPrecision, mYPrecision, downTime);
            getListener()->notifyMotion(&hoverArgs);
        }

        // Send scroll events.
        if (scrolled) {
            pointerCoords.setAxisValue(AMOTION_EVENT_AXIS_VSCROLL, vscroll);
            pointerCoords.setAxisValue(AMOTION_EVENT_AXIS_HSCROLL, hscroll);

            NotifyMotionArgs scrollArgs(when, getDeviceId(), mSource, policyFlags,
                    AMOTION_EVENT_ACTION_SCROLL, 0, metaState, currentButtonState,
                    AMOTION_EVENT_EDGE_FLAG_NONE,
                    1, &pointerProperties, &pointerCoords, mXPrecision, mYPrecision, downTime);
            getListener()->notifyMotion(&scrollArgs);
        }
    }

    // Synthesize key up from buttons if needed.
    synthesizeButtonKeys(getContext(), AKEY_EVENT_ACTION_UP, when, getDeviceId(), mSource,
            policyFlags, lastButtonState, currentButtonState);

    mCursorMotionAccumulator.finishSync();
    mCursorScrollAccumulator.finishSync();
}

int32_t CursorInputMapper::getScanCodeState(uint32_t sourceMask, int32_t scanCode) {
    if (scanCode >= BTN_MOUSE && scanCode < BTN_JOYSTICK) {
        return getEventHub()->getScanCodeState(getDeviceId(), scanCode);
    } else {
        return AKEY_STATE_UNKNOWN;
    }
}

void CursorInputMapper::fadePointer() {
    if (mPointerController != NULL) {
        mPointerController->fade(PointerControllerInterface::TRANSITION_GRADUAL);
    }
}


// --- TouchInputMapper ---

TouchInputMapper::TouchInputMapper(InputDevice* device) :
        InputMapper(device),
        mSource(0), mDeviceMode(DEVICE_MODE_DISABLED),
        mSurfaceOrientation(-1), mSurfaceWidth(-1), mSurfaceHeight(-1) {
}

TouchInputMapper::~TouchInputMapper() {
}

uint32_t TouchInputMapper::getSources() {
    return mSource;
}

void TouchInputMapper::populateDeviceInfo(InputDeviceInfo* info) {
    InputMapper::populateDeviceInfo(info);

    if (mDeviceMode != DEVICE_MODE_DISABLED) {
        info->addMotionRange(mOrientedRanges.x);
        info->addMotionRange(mOrientedRanges.y);
        info->addMotionRange(mOrientedRanges.pressure);

        if (mOrientedRanges.haveSize) {
            info->addMotionRange(mOrientedRanges.size);
        }

        if (mOrientedRanges.haveTouchSize) {
            info->addMotionRange(mOrientedRanges.touchMajor);
            info->addMotionRange(mOrientedRanges.touchMinor);
        }

        if (mOrientedRanges.haveToolSize) {
            info->addMotionRange(mOrientedRanges.toolMajor);
            info->addMotionRange(mOrientedRanges.toolMinor);
        }

        if (mOrientedRanges.haveOrientation) {
            info->addMotionRange(mOrientedRanges.orientation);
        }

        if (mOrientedRanges.haveDistance) {
            info->addMotionRange(mOrientedRanges.distance);
        }

        if (mOrientedRanges.haveTilt) {
            info->addMotionRange(mOrientedRanges.tilt);
        }

        if (mCursorScrollAccumulator.haveRelativeVWheel()) {
            info->addMotionRange(AMOTION_EVENT_AXIS_VSCROLL, mSource, -1.0f, 1.0f, 0.0f, 0.0f);
        }
        if (mCursorScrollAccumulator.haveRelativeHWheel()) {
            info->addMotionRange(AMOTION_EVENT_AXIS_HSCROLL, mSource, -1.0f, 1.0f, 0.0f, 0.0f);
        }
    }
}

void TouchInputMapper::dump(String8& dump) {
    dump.append(INDENT2 "Touch Input Mapper:\n");
    dumpParameters(dump);
    dumpVirtualKeys(dump);
    dumpRawPointerAxes(dump);
    dumpCalibration(dump);
    dumpSurface(dump);

    dump.appendFormat(INDENT3 "Translation and Scaling Factors:\n");
    dump.appendFormat(INDENT4 "XScale: %0.3f\n", mXScale);
    dump.appendFormat(INDENT4 "YScale: %0.3f\n", mYScale);
    dump.appendFormat(INDENT4 "XPrecision: %0.3f\n", mXPrecision);
    dump.appendFormat(INDENT4 "YPrecision: %0.3f\n", mYPrecision);
    dump.appendFormat(INDENT4 "GeometricScale: %0.3f\n", mGeometricScale);
    dump.appendFormat(INDENT4 "PressureScale: %0.3f\n", mPressureScale);
    dump.appendFormat(INDENT4 "SizeScale: %0.3f\n", mSizeScale);
    dump.appendFormat(INDENT4 "OrientationCenter: %0.3f\n", mOrientationCenter);
    dump.appendFormat(INDENT4 "OrientationScale: %0.3f\n", mOrientationScale);
    dump.appendFormat(INDENT4 "DistanceScale: %0.3f\n", mDistanceScale);
    dump.appendFormat(INDENT4 "HaveTilt: %s\n", toString(mHaveTilt));
    dump.appendFormat(INDENT4 "TiltXCenter: %0.3f\n", mTiltXCenter);
    dump.appendFormat(INDENT4 "TiltXScale: %0.3f\n", mTiltXScale);
    dump.appendFormat(INDENT4 "TiltYCenter: %0.3f\n", mTiltYCenter);
    dump.appendFormat(INDENT4 "TiltYScale: %0.3f\n", mTiltYScale);

    dump.appendFormat(INDENT3 "Last Button State: 0x%08x\n", mLastButtonState);

    dump.appendFormat(INDENT3 "Last Raw Touch: pointerCount=%d\n",
            mLastRawPointerData.pointerCount);
    for (uint32_t i = 0; i < mLastRawPointerData.pointerCount; i++) {
        const RawPointerData::Pointer& pointer = mLastRawPointerData.pointers[i];
        dump.appendFormat(INDENT4 "[%d]: id=%d, x=%d, y=%d, pressure=%d, "
                "touchMajor=%d, touchMinor=%d, toolMajor=%d, toolMinor=%d, "
                "orientation=%d, tiltX=%d, tiltY=%d, distance=%d, "
                "toolType=%d, isHovering=%s\n", i,
                pointer.id, pointer.x, pointer.y, pointer.pressure,
                pointer.touchMajor, pointer.touchMinor,
                pointer.toolMajor, pointer.toolMinor,
                pointer.orientation, pointer.tiltX, pointer.tiltY, pointer.distance,
                pointer.toolType, toString(pointer.isHovering));
    }

    dump.appendFormat(INDENT3 "Last Cooked Touch: pointerCount=%d\n",
            mLastCookedPointerData.pointerCount);
    for (uint32_t i = 0; i < mLastCookedPointerData.pointerCount; i++) {
        const PointerProperties& pointerProperties = mLastCookedPointerData.pointerProperties[i];
        const PointerCoords& pointerCoords = mLastCookedPointerData.pointerCoords[i];
        dump.appendFormat(INDENT4 "[%d]: id=%d, x=%0.3f, y=%0.3f, pressure=%0.3f, "
                "touchMajor=%0.3f, touchMinor=%0.3f, toolMajor=%0.3f, toolMinor=%0.3f, "
                "orientation=%0.3f, tilt=%0.3f, distance=%0.3f, "
                "toolType=%d, isHovering=%s\n", i,
                pointerProperties.id,
                pointerCoords.getX(),
                pointerCoords.getY(),
                pointerCoords.getAxisValue(AMOTION_EVENT_AXIS_PRESSURE),
                pointerCoords.getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MAJOR),
                pointerCoords.getAxisValue(AMOTION_EVENT_AXIS_TOUCH_MINOR),
                pointerCoords.getAxisValue(AMOTION_EVENT_AXIS_TOOL_MAJOR),
                pointerCoords.getAxisValue(AMOTION_EVENT_AXIS_TOOL_MINOR),
                pointerCoords.getAxisValue(AMOTION_EVENT_AXIS_ORIENTATION),
                pointerCoords.getAxisValue(AMOTION_EVENT_AXIS_TILT),
                pointerCoords.getAxisValue(AMOTION_EVENT_AXIS_DISTANCE),
                pointerProperties.toolType,
                toString(mLastCookedPointerData.isHovering(i)));
    }

    if (mDeviceMode == DEVICE_MODE_POINTER) {
        dump.appendFormat(INDENT3 "Pointer Gesture Detector:\n");
        dump.appendFormat(INDENT4 "XMovementScale: %0.3f\n",
                mPointerXMovementScale);
        dump.appendFormat(INDENT4 "YMovementScale: %0.3f\n",
                mPointerYMovementScale);
        dump.appendFormat(INDENT4 "XZoomScale: %0.3f\n",
                mPointerXZoomScale);
        dump.appendFormat(INDENT4 "YZoomScale: %0.3f\n",
                mPointerYZoomScale);
        dump.appendFormat(INDENT4 "MaxSwipeWidth: %f\n",
                mPointerGestureMaxSwipeWidth);
    }
}

void TouchInputMapper::configure(nsecs_t when,
        const InputReaderConfiguration* config, uint32_t changes) {
    InputMapper::configure(when, config, changes);

    mConfig = *config;

    if (!changes) { // first time only
        // Configure basic parameters.
        configureParameters();

        // Configure common accumulators.
        mCursorScrollAccumulator.configure(getDevice());
        mTouchButtonAccumulator.configure(getDevice());

        // Configure absolute axis information.
        configureRawPointerAxes();

        // Prepare input device calibration.
        parseCalibration();
        resolveCalibration();
    }

    if (!changes || (changes & InputReaderConfiguration::CHANGE_POINTER_SPEED)) {
        // Update pointer speed.
        mPointerVelocityControl.setParameters(mConfig.pointerVelocityControlParameters);
        mWheelXVelocityControl.setParameters(mConfig.wheelVelocityControlParameters);
        mWheelYVelocityControl.setParameters(mConfig.wheelVelocityControlParameters);
    }

    bool resetNeeded = false;
    if (!changes || (changes & (InputReaderConfiguration::CHANGE_DISPLAY_INFO
            | InputReaderConfiguration::CHANGE_POINTER_GESTURE_ENABLEMENT
            | InputReaderConfiguration::CHANGE_SHOW_TOUCHES
            | InputReaderConfiguration::CHANGE_STYLUS_ICON_ENABLED))) {
        // Configure device sources, surface dimensions, orientation and
        // scaling factors.
        configureSurface(when, &resetNeeded);
    }

    if (changes && resetNeeded) {
        // Send reset, unless this is the first time the device has been configured,
        // in which case the reader will call reset itself after all mappers are ready.
        getDevice()->notifyReset(when);
    }
}

void TouchInputMapper::configureParameters() {
    // Use the pointer presentation mode for devices that do not support distinct
    // multitouch.  The spot-based presentation relies on being able to accurately
    // locate two or more fingers on the touch pad.
    mParameters.gestureMode = getEventHub()->hasInputProperty(getDeviceId(), INPUT_PROP_SEMI_MT)
            ? Parameters::GESTURE_MODE_POINTER : Parameters::GESTURE_MODE_SPOTS;

    String8 gestureModeString;
    if (getDevice()->getConfiguration().tryGetProperty(String8("touch.gestureMode"),
            gestureModeString)) {
        if (gestureModeString == "pointer") {
            mParameters.gestureMode = Parameters::GESTURE_MODE_POINTER;
        } else if (gestureModeString == "spots") {
            mParameters.gestureMode = Parameters::GESTURE_MODE_SPOTS;
        } else if (gestureModeString != "default") {
            ALOGW("Invalid value for touch.gestureMode: '%s'", gestureModeString.string());
        }
    }

    if (getEventHub()->hasInputProperty(getDeviceId(), INPUT_PROP_DIRECT)) {
        // The device is a touch screen.
        mParameters.deviceType = Parameters::DEVICE_TYPE_TOUCH_SCREEN;
    } else if (getEventHub()->hasInputProperty(getDeviceId(), INPUT_PROP_POINTER)) {
        // The device is a pointing device like a track pad.
        mParameters.deviceType = Parameters::DEVICE_TYPE_POINTER;
    } else if (getEventHub()->hasRelativeAxis(getDeviceId(), REL_X)
            || getEventHub()->hasRelativeAxis(getDeviceId(), REL_Y)) {
        // The device is a cursor device with a touch pad attached.
        // By default don't use the touch pad to move the pointer.
        mParameters.deviceType = Parameters::DEVICE_TYPE_TOUCH_PAD;
    } else {
        // The device is a touch pad of unknown purpose.
        mParameters.deviceType = Parameters::DEVICE_TYPE_POINTER;
    }

    String8 deviceTypeString;
    if (getDevice()->getConfiguration().tryGetProperty(String8("touch.deviceType"),
            deviceTypeString)) {
        if (deviceTypeString == "touchScreen") {
            mParameters.deviceType = Parameters::DEVICE_TYPE_TOUCH_SCREEN;
        } else if (deviceTypeString == "touchPad") {
            mParameters.deviceType = Parameters::DEVICE_TYPE_TOUCH_PAD;
        } else if (deviceTypeString == "pointer") {
            mParameters.deviceType = Parameters::DEVICE_TYPE_POINTER;
        } else if (deviceTypeString != "default") {
            ALOGW("Invalid value for touch.deviceType: '%s'", deviceTypeString.string());
        }
    }

    mParameters.orientationAware = mParameters.deviceType == Parameters::DEVICE_TYPE_TOUCH_SCREEN;
    getDevice()->getConfiguration().tryGetProperty(String8("touch.orientationAware"),
            mParameters.orientationAware);

    mParameters.associatedDisplayId = -1;
    mParameters.associatedDisplayIsExternal = false;
    if (mParameters.orientationAware
            || mParameters.deviceType == Parameters::DEVICE_TYPE_TOUCH_SCREEN
            || mParameters.deviceType == Parameters::DEVICE_TYPE_POINTER) {
        mParameters.associatedDisplayIsExternal =
                mParameters.deviceType == Parameters::DEVICE_TYPE_TOUCH_SCREEN
                        && getDevice()->isExternal();
        mParameters.associatedDisplayId = 0;
    }
}

void TouchInputMapper::dumpParameters(String8& dump) {
    dump.append(INDENT3 "Parameters:\n");

    switch (mParameters.gestureMode) {
    case Parameters::GESTURE_MODE_POINTER:
        dump.append(INDENT4 "GestureMode: pointer\n");
        break;
    case Parameters::GESTURE_MODE_SPOTS:
        dump.append(INDENT4 "GestureMode: spots\n");
        break;
    default:
        assert(false);
    }

    switch (mParameters.deviceType) {
    case Parameters::DEVICE_TYPE_TOUCH_SCREEN:
        dump.append(INDENT4 "DeviceType: touchScreen\n");
        break;
    case Parameters::DEVICE_TYPE_TOUCH_PAD:
        dump.append(INDENT4 "DeviceType: touchPad\n");
        break;
    case Parameters::DEVICE_TYPE_POINTER:
        dump.append(INDENT4 "DeviceType: pointer\n");
        break;
    default:
        ALOG_ASSERT(false);
    }

    dump.appendFormat(INDENT4 "AssociatedDisplay: id=%d, isExternal=%s\n",
            mParameters.associatedDisplayId, toString(mParameters.associatedDisplayIsExternal));
    dump.appendFormat(INDENT4 "OrientationAware: %s\n",
            toString(mParameters.orientationAware));
}

void TouchInputMapper::configureRawPointerAxes() {
    mRawPointerAxes.clear();
}

void TouchInputMapper::dumpRawPointerAxes(String8& dump) {
    dump.append(INDENT3 "Raw Touch Axes:\n");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.x, "X");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.y, "Y");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.pressure, "Pressure");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.touchMajor, "TouchMajor");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.touchMinor, "TouchMinor");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.toolMajor, "ToolMajor");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.toolMinor, "ToolMinor");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.orientation, "Orientation");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.distance, "Distance");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.tiltX, "TiltX");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.tiltY, "TiltY");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.trackingId, "TrackingId");
    dumpRawAbsoluteAxisInfo(dump, mRawPointerAxes.slot, "Slot");
}

void TouchInputMapper::configureSurface(nsecs_t when, bool* outResetNeeded) {
    int32_t oldDeviceMode = mDeviceMode;

    // Determine device mode.
    if (mParameters.deviceType == Parameters::DEVICE_TYPE_POINTER
            && mConfig.pointerGesturesEnabled) {
        mSource = AINPUT_SOURCE_MOUSE;
        mDeviceMode = DEVICE_MODE_POINTER;
        if (hasStylus()) {
            mSource |= AINPUT_SOURCE_STYLUS;
        }
    } else if (mParameters.deviceType == Parameters::DEVICE_TYPE_TOUCH_SCREEN
            && mParameters.associatedDisplayId >= 0) {
        mSource = AINPUT_SOURCE_TOUCHSCREEN;
        mDeviceMode = DEVICE_MODE_DIRECT;
        if (hasStylus()) {
            mSource |= AINPUT_SOURCE_STYLUS;
        }
    } else {
        mSource = AINPUT_SOURCE_TOUCHPAD;
        mDeviceMode = DEVICE_MODE_UNSCALED;
    }

    // Ensure we have valid X and Y axes.
    if (!mRawPointerAxes.x.valid || !mRawPointerAxes.y.valid) {
        ALOGW(INDENT "Touch device '%s' did not report support for X or Y axis!  "
                "The device will be inoperable.", getDeviceName().string());
        mDeviceMode = DEVICE_MODE_DISABLED;
        return;
    }

    // Get associated display dimensions.
    if (mParameters.associatedDisplayId >= 0) {
        if (!mConfig.getDisplayInfo(mParameters.associatedDisplayId,
                mParameters.associatedDisplayIsExternal,
                &mAssociatedDisplayWidth, &mAssociatedDisplayHeight,
                &mAssociatedDisplayOrientation)) {
            ALOGI(INDENT "Touch device '%s' could not query the properties of its associated "
                    "display %d.  The device will be inoperable until the display size "
                    "becomes available.",
                    getDeviceName().string(), mParameters.associatedDisplayId);
            mDeviceMode = DEVICE_MODE_DISABLED;
            return;
        }
    }

    // Configure dimensions.
    int32_t width, height, orientation;
    if (mDeviceMode == DEVICE_MODE_DIRECT || mDeviceMode == DEVICE_MODE_POINTER) {
        width = mAssociatedDisplayWidth;
        height = mAssociatedDisplayHeight;
        orientation = mParameters.orientationAware ?
                mAssociatedDisplayOrientation : DISPLAY_ORIENTATION_0;
    } else {
        width = mRawPointerAxes.x.maxValue - mRawPointerAxes.x.minValue + 1;
        height = mRawPointerAxes.y.maxValue - mRawPointerAxes.y.minValue + 1;
        orientation = DISPLAY_ORIENTATION_0;
    }

    // If moving between pointer modes, need to reset some state.
    bool deviceModeChanged;
    if (mDeviceMode != oldDeviceMode) {
        deviceModeChanged = true;
        mOrientedRanges.clear();
    }

    // Create pointer controller if needed.
    if (mDeviceMode == DEVICE_MODE_POINTER ||
            (mDeviceMode == DEVICE_MODE_DIRECT && mConfig.showTouches)) {
        if (mPointerController == NULL) {
            mPointerController = getPolicy()->obtainPointerController(getDeviceId());
        }
    } else {
        mPointerController.clear();
    }

    bool orientationChanged = mSurfaceOrientation != orientation;
    if (orientationChanged) {
        mSurfaceOrientation = orientation;
    }

    bool sizeChanged = mSurfaceWidth != width || mSurfaceHeight != height;
    if (sizeChanged || deviceModeChanged) {
        ALOGI("Device reconfigured: id=%d, name='%s', surface size is now %dx%d, mode is %d",
                getDeviceId(), getDeviceName().string(), width, height, mDeviceMode);

        mSurfaceWidth = width;
        mSurfaceHeight = height;

        // Configure X and Y factors.
        mXScale = float(width) / (mRawPointerAxes.x.maxValue - mRawPointerAxes.x.minValue + 1);
        mYScale = float(height) / (mRawPointerAxes.y.maxValue - mRawPointerAxes.y.minValue + 1);
        mXPrecision = 1.0f / mXScale;
        mYPrecision = 1.0f / mYScale;

        mOrientedRanges.x.axis = AMOTION_EVENT_AXIS_X;
        mOrientedRanges.x.source = mSource;
        mOrientedRanges.y.axis = AMOTION_EVENT_AXIS_Y;
        mOrientedRanges.y.source = mSource;

        configureVirtualKeys();

        // Scale factor for terms that are not oriented in a particular axis.
        // If the pixels are square then xScale == yScale otherwise we fake it
        // by choosing an average.
        mGeometricScale = avg(mXScale, mYScale);

        // Size of diagonal axis.
        float diagonalSize = hypotf(width, height);

        // Size factors.
        if (mCalibration.sizeCalibration != Calibration::SIZE_CALIBRATION_NONE) {
            if (mRawPointerAxes.touchMajor.valid
                    && mRawPointerAxes.touchMajor.maxValue != 0) {
                mSizeScale = 1.0f / mRawPointerAxes.touchMajor.maxValue;
            } else if (mRawPointerAxes.toolMajor.valid
                    && mRawPointerAxes.toolMajor.maxValue != 0) {
                mSizeScale = 1.0f / mRawPointerAxes.toolMajor.maxValue;
            } else {
                mSizeScale = 0.0f;
            }

            mOrientedRanges.haveTouchSize = true;
            mOrientedRanges.haveToolSize = true;
            mOrientedRanges.haveSize = true;

            mOrientedRanges.touchMajor.axis = AMOTION_EVENT_AXIS_TOUCH_MAJOR;
            mOrientedRanges.touchMajor.source = mSource;
            mOrientedRanges.touchMajor.min = 0;
            mOrientedRanges.touchMajor.max = diagonalSize;
            mOrientedRanges.touchMajor.flat = 0;
            mOrientedRanges.touchMajor.fuzz = 0;

            mOrientedRanges.touchMinor = mOrientedRanges.touchMajor;
            mOrientedRanges.touchMinor.axis = AMOTION_EVENT_AXIS_TOUCH_MINOR;

            mOrientedRanges.toolMajor.axis = AMOTION_EVENT_AXIS_TOOL_MAJOR;
            mOrientedRanges.toolMajor.source = mSource;
            mOrientedRanges.toolMajor.min = 0;
            mOrientedRanges.toolMajor.max = diagonalSize;
            mOrientedRanges.toolMajor.flat = 0;
            mOrientedRanges.toolMajor.fuzz = 0;

            mOrientedRanges.toolMinor = mOrientedRanges.toolMajor;
            mOrientedRanges.toolMinor.axis = AMOTION_EVENT_AXIS_TOOL_MINOR;

            mOrientedRanges.size.axis = AMOTION_EVENT_AXIS_SIZE;
            mOrientedRanges.size.source = mSource;
            mOrientedRanges.size.min = 0;
            mOrientedRanges.size.max = 1.0;
            mOrientedRanges.size.flat = 0;
            mOrientedRanges.size.fuzz = 0;
        } else {
            mSizeScale = 0.0f;
        }

        // Pressure factors.
        mPressureScale = 0;
        if (mCalibration.pressureCalibration == Calibration::PRESSURE_CALIBRATION_PHYSICAL
                || mCalibration.pressureCalibration
                        == Calibration::PRESSURE_CALIBRATION_AMPLITUDE) {
            if (mCalibration.havePressureScale) {
                mPressureScale = mCalibration.pressureScale;
            } else if (mRawPointerAxes.pressure.valid
                    && mRawPointerAxes.pressure.maxValue != 0) {
                mPressureScale = 1.0f / mRawPointerAxes.pressure.maxValue;
            }
        }

        mOrientedRanges.pressure.axis = AMOTION_EVENT_AXIS_PRESSURE;
        mOrientedRanges.pressure.source = mSource;
        mOrientedRanges.pressure.min = 0;
        mOrientedRanges.pressure.max = 1.0;
        mOrientedRanges.pressure.flat = 0;
        mOrientedRanges.pressure.fuzz = 0;

        // Tilt
        mTiltXCenter = 0;
        mTiltXScale = 0;
        mTiltYCenter = 0;
        mTiltYScale = 0;
        mHaveTilt = mRawPointerAxes.tiltX.valid && mRawPointerAxes.tiltY.valid;
        if (mHaveTilt) {
            mTiltXCenter = avg(mRawPointerAxes.tiltX.minValue,
                    mRawPointerAxes.tiltX.maxValue);
            mTiltYCenter = avg(mRawPointerAxes.tiltY.minValue,
                    mRawPointerAxes.tiltY.maxValue);
            mTiltXScale = M_PI / 180;
            mTiltYScale = M_PI / 180;

            mOrientedRanges.haveTilt = true;

            mOrientedRanges.tilt.axis = AMOTION_EVENT_AXIS_TILT;
            mOrientedRanges.tilt.source = mSource;
            mOrientedRanges.tilt.min = 0;
            mOrientedRanges.tilt.max = M_PI_2;
            mOrientedRanges.tilt.flat = 0;
            mOrientedRanges.tilt.fuzz = 0;
        }

        // Orientation
        mOrientationCenter = 0;
        mOrientationScale = 0;
        if (mHaveTilt) {
            mOrientedRanges.haveOrientation = true;

            mOrientedRanges.orientation.axis = AMOTION_EVENT_AXIS_ORIENTATION;
            mOrientedRanges.orientation.source = mSource;
            mOrientedRanges.orientation.min = -M_PI;
            mOrientedRanges.orientation.max = M_PI;
            mOrientedRanges.orientation.flat = 0;
            mOrientedRanges.orientation.fuzz = 0;
        } else if (mCalibration.orientationCalibration !=
                Calibration::ORIENTATION_CALIBRATION_NONE) {
            if (mCalibration.orientationCalibration
                    == Calibration::ORIENTATION_CALIBRATION_INTERPOLATED) {
                if (mRawPointerAxes.orientation.valid) {
                    mOrientationCenter = avg(mRawPointerAxes.orientation.minValue,
                            mRawPointerAxes.orientation.maxValue);
                    mOrientationScale = M_PI / (mRawPointerAxes.orientation.maxValue -
                            mRawPointerAxes.orientation.minValue);
                }
            }

            mOrientedRanges.haveOrientation = true;

            mOrientedRanges.orientation.axis = AMOTION_EVENT_AXIS_ORIENTATION;
            mOrientedRanges.orientation.source = mSource;
            mOrientedRanges.orientation.min = -M_PI_2;
            mOrientedRanges.orientation.max = M_PI_2;
            mOrientedRanges.orientation.flat = 0;
            mOrientedRanges.orientation.fuzz = 0;
        }

        // Distance
        mDistanceScale = 0;
        if (mCalibration.distanceCalibration != Calibration::DISTANCE_CALIBRATION_NONE) {
            if (mCalibration.distanceCalibration
                    == Calibration::DISTANCE_CALIBRATION_SCALED) {
                if (mCalibration.haveDistanceScale) {
                    mDistanceScale = mCalibration.distanceScale;
                } else {
                    mDistanceScale = 1.0f;
                }
            }

            mOrientedRanges.haveDistance = true;

            mOrientedRanges.distance.axis = AMOTION_EVENT_AXIS_DISTANCE;
            mOrientedRanges.distance.source = mSource;
            mOrientedRanges.distance.min =
                    mRawPointerAxes.distance.minValue * mDistanceScale;
            mOrientedRanges.distance.max =
                    mRawPointerAxes.distance.maxValue * mDistanceScale;
            mOrientedRanges.distance.flat = 0;
            mOrientedRanges.distance.fuzz =
                    mRawPointerAxes.distance.fuzz * mDistanceScale;
        }
    }

    if (orientationChanged || sizeChanged || deviceModeChanged) {
        // Compute oriented surface dimensions, precision, scales and ranges.
        // Note that the maximum value reported is an inclusive maximum value so it is one
        // unit less than the total width or height of surface.
        switch (mSurfaceOrientation) {
        case DISPLAY_ORIENTATION_90:
        case DISPLAY_ORIENTATION_270:
            mOrientedSurfaceWidth = mSurfaceHeight;
            mOrientedSurfaceHeight = mSurfaceWidth;

            mOrientedXPrecision = mYPrecision;
            mOrientedYPrecision = mXPrecision;

            mOrientedRanges.x.min = 0;
            mOrientedRanges.x.max = (mRawPointerAxes.y.maxValue - mRawPointerAxes.y.minValue)
                    * mYScale;
            mOrientedRanges.x.flat = 0;
            mOrientedRanges.x.fuzz = mYScale;

            mOrientedRanges.y.min = 0;
            mOrientedRanges.y.max = (mRawPointerAxes.x.maxValue - mRawPointerAxes.x.minValue)
                    * mXScale;
            mOrientedRanges.y.flat = 0;
            mOrientedRanges.y.fuzz = mXScale;
            break;

        default:
            mOrientedSurfaceWidth = mSurfaceWidth;
            mOrientedSurfaceHeight = mSurfaceHeight;

            mOrientedXPrecision = mXPrecision;
            mOrientedYPrecision = mYPrecision;

            mOrientedRanges.x.min = 0;
            mOrientedRanges.x.max = (mRawPointerAxes.x.maxValue - mRawPointerAxes.x.minValue)
                    * mXScale;
            mOrientedRanges.x.flat = 0;
            mOrientedRanges.x.fuzz = mXScale;

            mOrientedRanges.y.min = 0;
            mOrientedRanges.y.max = (mRawPointerAxes.y.maxValue - mRawPointerAxes.y.minValue)
                    * mYScale;
            mOrientedRanges.y.flat = 0;
            mOrientedRanges.y.fuzz = mYScale;
            break;
        }

        // Compute pointer gesture detection parameters.
        if (mDeviceMode == DEVICE_MODE_POINTER) {
            int32_t rawWidth = mRawPointerAxes.x.maxValue - mRawPointerAxes.x.minValue + 1;
            int32_t rawHeight = mRawPointerAxes.y.maxValue - mRawPointerAxes.y.minValue + 1;
            float rawDiagonal = hypotf(rawWidth, rawHeight);
            float displayDiagonal = hypotf(mAssociatedDisplayWidth,
                    mAssociatedDisplayHeight);

            // Scale movements such that one whole swipe of the touch pad covers a
            // given area relative to the diagonal size of the display when no acceleration
            // is applied.
            // Assume that the touch pad has a square aspect ratio such that movements in
            // X and Y of the same number of raw units cover the same physical distance.
            mPointerXMovementScale = mConfig.pointerGestureMovementSpeedRatio
                    * displayDiagonal / rawDiagonal;
            mPointerYMovementScale = mPointerXMovementScale;

            // Scale zooms to cover a smaller range of the display than movements do.
            // This value determines the area around the pointer that is affected by freeform
            // pointer gestures.
            mPointerXZoomScale = mConfig.pointerGestureZoomSpeedRatio
                    * displayDiagonal / rawDiagonal;
            mPointerYZoomScale = mPointerXZoomScale;

            // Max width between pointers to detect a swipe gesture is more than some fraction
            // of the diagonal axis of the touch pad.  Touches that are wider than this are
            // translated into freeform gestures.
            mPointerGestureMaxSwipeWidth =
                    mConfig.pointerGestureSwipeMaxWidthRatio * rawDiagonal;
        }

        // Abort current pointer usages because the state has changed.
        abortPointerUsage(when, 0 /*policyFlags*/);

        // Inform the dispatcher about the changes.
        *outResetNeeded = true;
        bumpGeneration();
    }
}

void TouchInputMapper::dumpSurface(String8& dump) {
    dump.appendFormat(INDENT3 "SurfaceWidth: %dpx\n", mSurfaceWidth);
    dump.appendFormat(INDENT3 "SurfaceHeight: %dpx\n", mSurfaceHeight);
    dump.appendFormat(INDENT3 "SurfaceOrientation: %d\n", mSurfaceOrientation);
}

void TouchInputMapper::configureVirtualKeys() {
    Vector<VirtualKeyDefinition> virtualKeyDefinitions;
    getEventHub()->getVirtualKeyDefinitions(getDeviceId(), virtualKeyDefinitions);

    mVirtualKeys.clear();

    if (virtualKeyDefinitions.size() == 0) {
        return;
    }

    mVirtualKeys.setCapacity(virtualKeyDefinitions.size());

    int32_t touchScreenLeft = mRawPointerAxes.x.minValue;
    int32_t touchScreenTop = mRawPointerAxes.y.minValue;
    int32_t touchScreenWidth = mRawPointerAxes.x.maxValue - mRawPointerAxes.x.minValue + 1;
    int32_t touchScreenHeight = mRawPointerAxes.y.maxValue - mRawPointerAxes.y.minValue + 1;

    for (size_t i = 0; i < virtualKeyDefinitions.size(); i++) {
        const VirtualKeyDefinition& virtualKeyDefinition =
                virtualKeyDefinitions[i];

        mVirtualKeys.add();
        VirtualKey& virtualKey = mVirtualKeys.editTop();

        virtualKey.scanCode = virtualKeyDefinition.scanCode;
        int32_t keyCode;
        uint32_t flags;
        if (getEventHub()->mapKey(getDeviceId(), virtualKey.scanCode, 0, &keyCode, &flags)) {
            ALOGW(INDENT "VirtualKey %d: could not obtain key code, ignoring",
                    virtualKey.scanCode);
            mVirtualKeys.pop(); // drop the key
            continue;
        }

        virtualKey.keyCode = keyCode;
        virtualKey.flags = flags;

        // convert the key definition's display coordinates into touch coordinates for a hit box
        int32_t halfWidth = virtualKeyDefinition.width / 2;
        int32_t halfHeight = virtualKeyDefinition.height / 2;

        virtualKey.hitLeft = (virtualKeyDefinition.centerX - halfWidth)
                * touchScreenWidth / mSurfaceWidth + touchScreenLeft;
        virtualKey.hitRight= (virtualKeyDefinition.centerX + halfWidth)
                * touchScreenWidth / mSurfaceWidth + touchScreenLeft;
        virtualKey.hitTop = (virtualKeyDefinition.centerY - halfHeight)
                * touchScreenHeight / mSurfaceHeight + touchScreenTop;
        virtualKey.hitBottom = (virtualKeyDefinition.centerY + halfHeight)
                * touchScreenHeight / mSurfaceHeight + touchScreenTop;
    }
}

void TouchInputMapper::dumpVirtualKeys(String8& dump) {
    if (!mVirtualKeys.isEmpty()) {
        dump.append(INDENT3 "Virtual Keys:\n");

        for (size_t i = 0; i < mVirtualKeys.size(); i++) {
            const VirtualKey& virtualKey = mVirtualKeys.itemAt(i);
            dump.appendFormat(INDENT4 "%d: scanCode=%d, keyCode=%d, "
                    "hitLeft=%d, hitRight=%d, hitTop=%d, hitBottom=%d\n",
                    i, virtualKey.scanCode, virtualKey.keyCode,
                    virtualKey.hitLeft, virtualKey.hitRight,
                    virtualKey.hitTop, virtualKey.hitBottom);
        }
    }
}

void TouchInputMapper::parseCalibration() {
    const PropertyMap& in = getDevice()->getConfiguration();
    Calibration& out = mCalibration;

    // Size
    out.sizeCalibration = Calibration::SIZE_CALIBRATION_DEFAULT;
    String8 sizeCalibrationString;
    if (in.tryGetProperty(String8("touch.size.calibration"), sizeCalibrationString)) {
        if (sizeCalibrationString == "none") {
            out.sizeCalibration = Calibration::SIZE_CALIBRATION_NONE;
        } else if (sizeCalibrationString == "geometric") {
            out.sizeCalibration = Calibration::SIZE_CALIBRATION_GEOMETRIC;
        } else if (sizeCalibrationString == "diameter") {
            out.sizeCalibration = Calibration::SIZE_CALIBRATION_DIAMETER;
        } else if (sizeCalibrationString == "area") {
            out.sizeCalibration = Calibration::SIZE_CALIBRATION_AREA;
        } else if (sizeCalibrationString != "default") {
            ALOGW("Invalid value for touch.size.calibration: '%s'",
                    sizeCalibrationString.string());
        }
    }

    out.haveSizeScale = in.tryGetProperty(String8("touch.size.scale"),
            out.sizeScale);
    out.haveSizeBias = in.tryGetProperty(String8("touch.size.bias"),
            out.sizeBias);
    out.haveSizeIsSummed = in.tryGetProperty(String8("touch.size.isSummed"),
            out.sizeIsSummed);

    // Pressure
    out.pressureCalibration = Calibration::PRESSURE_CALIBRATION_DEFAULT;
    String8 pressureCalibrationString;
    if (in.tryGetProperty(String8("touch.pressure.calibration"), pressureCalibrationString)) {
        if (pressureCalibrationString == "none") {
            out.pressureCalibration = Calibration::PRESSURE_CALIBRATION_NONE;
        } else if (pressureCalibrationString == "physical") {
            out.pressureCalibration = Calibration::PRESSURE_CALIBRATION_PHYSICAL;
        } else if (pressureCalibrationString == "amplitude") {
            out.pressureCalibration = Calibration::PRESSURE_CALIBRATION_AMPLITUDE;
        } else if (pressureCalibrationString != "default") {
            ALOGW("Invalid value for touch.pressure.calibration: '%s'",
                    pressureCalibrationString.string());
        }
    }

    out.havePressureScale = in.tryGetProperty(String8("touch.pressure.scale"),
            out.pressureScale);

    // Orientation
    out.orientationCalibration = Calibration::ORIENTATION_CALIBRATION_DEFAULT;
    String8 orientationCalibrationString;
    if (in.tryGetProperty(String8("touch.orientation.calibration"), orientationCalibrationString)) {
        if (orientationCalibrationString == "none") {
            out.orientationCalibration = Calibration::ORIENTATION_CALIBRATION_NONE;
        } else if (orientationCalibrationString == "interpolated") {
            out.orientationCalibration = Calibration::ORIENTATION_CALIBRATION_INTERPOLATED;
        } else if (orientationCalibrationString == "vector") {
            out.orientationCalibration = Calibration::ORIENTATION_CALIBRATION_VECTOR;
        } else if (orientationCalibrationString != "default") {
            ALOGW("Invalid value for touch.orientation.calibration: '%s'",
                    orientationCalibrationString.string());
        }
    }

    // Distance
    out.distanceCalibration = Calibration::DISTANCE_CALIBRATION_DEFAULT;
    String8 distanceCalibrationString;
    if (in.tryGetProperty(String8("touch.distance.calibration"), distanceCalibrationString)) {
        if (distanceCalibrationString == "none") {
            out.distanceCalibration = Calibration::DISTANCE_CALIBRATION_NONE;
        } else if (distanceCalibrationString == "scaled") {
            out.distanceCalibration = Calibration::DISTANCE_CALIBRATION_SCALED;
        } else if (distanceCalibrationString != "default") {
            ALOGW("Invalid value for touch.distance.calibration: '%s'",
                    distanceCalibrationString.string());
        }
    }

    out.haveDistanceScale = in.tryGetProperty(String8("touch.distance.scale"),
            out.distanceScale);
}

void TouchInputMapper::resolveCalibration() {
    // Size
    if (mRawPointerAxes.touchMajor.valid || mRawPointerAxes.toolMajor.valid) {
        if (mCalibration.sizeCalibration == Calibration::SIZE_CALIBRATION_DEFAULT) {
            mCalibration.sizeCalibration = Calibration::SIZE_CALIBRATION_GEOMETRIC;
        }
    } else {
        mCalibration.sizeCalibration = Calibration::SIZE_CALIBRATION_NONE;
    }

    // Pressure
    if (mRawPointerAxes.pressure.valid) {
        if (mCalibration.pressureCalibration == Calibration::PRESSURE_CALIBRATION_DEFAULT) {
            mCalibration.pressureCalibration = Calibration::PRESSURE_CALIBRATION_PHYSICAL;
        }
    } else {
        mCalibration.pressureCalibration = Calibration::PRESSURE_CALIBRATION_NONE;
    }

    // Orientation
    if (mRawPointerAxes.orientation.valid) {
        if (mCalibration.orientationCalibration == Calibration::ORIENTATION_CALIBRATION_DEFAULT) {
            mCalibration.orientationCalibration = Calibration::ORIENTATION_CALIBRATION_INTERPOLATED;
        }
    } else {
        mCalibration.orientationCalibration = Calibration::ORIENTATION_CALIBRATION_NONE;
    }

    // Distance
    if (mRawPointerAxes.distance.valid) {
        if (mCalibration.distanceCalibration == Calibration::DISTANCE_CALIBRATION_DEFAULT) {
            mCalibration.distanceCalibration = Calibration::DISTANCE_CALIBRATION_SCALED;
        }
    } else {
        mCalibration.distanceCalibration = Calibration::DISTANCE_CALIBRATION_NONE;
    }
}

void TouchInputMapper::dumpCalibration(String8& dump) {
    dump.append(INDENT3 "Calibration:\n");

    // Size
    switch (mCalibration.sizeCalibration) {
    case Calibration::SIZE_CALIBRATION_NONE:
        dump.append(INDENT4 "touch.size.calibration: none\n");
        break;
    case Calibration::SIZE_CALIBRATION_GEOMETRIC:
        dump.append(INDENT4 "touch.size.calibration: geometric\n");
        break;
    case Calibration::SIZE_CALIBRATION_DIAMETER:
        dump.append(INDENT4 "touch.size.calibration: diameter\n");
        break;
    case Calibration::SIZE_CALIBRATION_AREA:
        dump.append(INDENT4 "touch.size.calibration: area\n");
        break;
    default:
        ALOG_ASSERT(false);
    }

    if (mCalibration.haveSizeScale) {
        dump.appendFormat(INDENT4 "touch.size.scale: %0.3f\n",
                mCalibration.sizeScale);
    }

    if (mCalibration.haveSizeBias) {
        dump.appendFormat(INDENT4 "touch.size.bias: %0.3f\n",
                mCalibration.sizeBias);
    }

    if (mCalibration.haveSizeIsSummed) {
        dump.appendFormat(INDENT4 "touch.size.isSummed: %s\n",
                toString(mCalibration.sizeIsSummed));
    }

    // Pressure
    switch (mCalibration.pressureCalibration) {
    case Calibration::PRESSURE_CALIBRATION_NONE:
        dump.append(INDENT4 "touch.pressure.calibration: none\n");
        break;
    case Calibration::PRESSURE_CALIBRATION_PHYSICAL:
        dump.append(INDENT4 "touch.pressure.calibration: physical\n");
        break;
    case Calibration::PRESSURE_CALIBRATION_AMPLITUDE:
        dump.append(INDENT4 "touch.pressure.calibration: amplitude\n");
        break;
    default:
        ALOG_ASSERT(false);
    }

    if (mCalibration.havePressureScale) {
        dump.appendFormat(INDENT4 "touch.pressure.scale: %0.3f\n",
                mCalibration.pressureScale);
    }

    // Orientation
    switch (mCalibration.orientationCalibration) {
    case Calibration::ORIENTATION_CALIBRATION_NONE:
        dump.append(INDENT4 "touch.orientation.calibration: none\n");
        break;
    case Calibration::ORIENTATION_CALIBRATION_INTERPOLATED:
        dump.append(INDENT4 "touch.orientation.calibration: interpolated\n");
        break;
    case Calibration::ORIENTATION_CALIBRATION_VECTOR:
        dump.append(INDENT4 "touch.orientation.calibration: vector\n");
        break;
    default:
        ALOG_ASSERT(false);
    }

    // Distance
    switch (mCalibration.distanceCalibration) {
    case Calibration::DISTANCE_CALIBRATION_NONE:
        dump.append(INDENT4 "touch.distance.calibration: none\n");
        break;
    case Calibration::DISTANCE_CALIBRATION_SCALED:
        dump.append(INDENT4 "touch.distance.calibration: scaled\n");
        break;
    default:
        ALOG_ASSERT(false);
    }

    if (mCalibration.haveDistanceScale) {
        dump.appendFormat(INDENT4 "touch.distance.scale: %0.3f\n",
                mCalibration.distanceScale);
    }
}

void TouchInputMapper::reset(nsecs_t when) {
    mCursorButtonAccumulator.reset(getDevice());
    mCursorScrollAccumulator.reset(getDevice());
    mTouchButtonAccumulator.reset(getDevice());

    mPointerVelocityControl.reset();
    mWheelXVelocityControl.reset();
    mWheelYVelocityControl.reset();

    mCurrentRawPointerData.clear();
    mLastRawPointerData.clear();
    mCurrentCookedPointerData.clear();
    mLastCookedPointerData.clear();
    mCurrentButtonState = 0;
    mLastButtonState = 0;
    mCurrentRawVScroll = 0;
    mCurrentRawHScroll = 0;
    mCurrentFingerIdBits.clear();
    mLastFingerIdBits.clear();
    mCurrentStylusIdBits.clear();
    mLastStylusIdBits.clear();
    mCurrentMouseIdBits.clear();
    mLastMouseIdBits.clear();
    mPointerUsage = POINTER_USAGE_NONE;
    mSentHoverEnter = false;
    mDownTime = 0;

    mCurrentVirtualKey.down = false;

    mPointerGesture.reset();
    mPointerSimple.reset();

    if (mPointerController != NULL) {
        mPointerController->fade(PointerControllerInterface::TRANSITION_GRADUAL);
        mPointerController->clearSpots();
    }

    InputMapper::reset(when);
}

void TouchInputMapper::process(const RawEvent* rawEvent) {
    mCursorButtonAccumulator.process(rawEvent);
    mCursorScrollAccumulator.process(rawEvent);
    mTouchButtonAccumulator.process(rawEvent);

    if (rawEvent->type == EV_SYN && rawEvent->code == SYN_REPORT) {
        sync(rawEvent->when);
    }
}

void TouchInputMapper::sync(nsecs_t when) {
    // Sync button state.
    mCurrentButtonState = mTouchButtonAccumulator.getButtonState()
            | mCursorButtonAccumulator.getButtonState();

    // Sync scroll state.
    mCurrentRawVScroll = mCursorScrollAccumulator.getRelativeVWheel();
    mCurrentRawHScroll = mCursorScrollAccumulator.getRelativeHWheel();
    mCursorScrollAccumulator.finishSync();

    // Sync touch state.
    bool havePointerIds = true;
    mCurrentRawPointerData.clear();
    syncTouch(when, &havePointerIds);

#if DEBUG_RAW_EVENTS
    if (!havePointerIds) {
        ALOGD("syncTouch: pointerCount %d -> %d, no pointer ids",
                mLastRawPointerData.pointerCount,
                mCurrentRawPointerData.pointerCount);
    } else {
        ALOGD("syncTouch: pointerCount %d -> %d, touching ids 0x%08x -> 0x%08x, "
                "hovering ids 0x%08x -> 0x%08x",
                mLastRawPointerData.pointerCount,
                mCurrentRawPointerData.pointerCount,
                mLastRawPointerData.touchingIdBits.value,
                mCurrentRawPointerData.touchingIdBits.value,
                mLastRawPointerData.hoveringIdBits.value,
                mCurrentRawPointerData.hoveringIdBits.value);
    }
#endif

    // Reset state that we will compute below.
    mCurrentFingerIdBits.clear();
    mCurrentStylusIdBits.clear();
    mCurrentMouseIdBits.clear();
    mCurrentCookedPointerData.clear();

    if (mDeviceMode == DEVICE_MODE_DISABLED) {
        // Drop all input if the device is disabled.
        mCurrentRawPointerData.clear();
        mCurrentButtonState = 0;
    } else {
        // Preprocess pointer data.
        if (!havePointerIds) {
            assignPointerIds();
        }

        // Handle policy on initial down or hover events.
        uint32_t policyFlags = 0;
        bool initialDown = mLastRawPointerData.pointerCount == 0
                && mCurrentRawPointerData.pointerCount != 0;
        bool buttonsPressed = mCurrentButtonState & ~mLastButtonState;
        if (initialDown || buttonsPressed) {
            // If this is a touch screen, hide the pointer on an initial down.
            if (mDeviceMode == DEVICE_MODE_DIRECT) {
                getContext()->fadePointer();
            }

            // Initial downs on external touch devices should wake the device.
            // We don't do this for internal touch screens to prevent them from waking
            // up in your pocket.
            // TODO: Use the input device configuration to control this behavior more finely.
            if (getDevice()->isExternal()) {
                policyFlags |= POLICY_FLAG_WAKE_DROPPED;
            }
        }

        // Synthesize key down from raw buttons if needed.
        synthesizeButtonKeys(getContext(), AKEY_EVENT_ACTION_DOWN, when, getDeviceId(), mSource,
                policyFlags, mLastButtonState, mCurrentButtonState);

        // Consume raw off-screen touches before cooking pointer data.
        // If touches are consumed, subsequent code will not receive any pointer data.
        if (consumeRawTouches(when, policyFlags)) {
            mCurrentRawPointerData.clear();
        }

        // Cook pointer data.  This call populates the mCurrentCookedPointerData structure
        // with cooked pointer data that has the same ids and indices as the raw data.
        // The following code can use either the raw or cooked data, as needed.
        cookPointerData();

        // Dispatch the touches either directly or by translation through a pointer on screen.
        if (mDeviceMode == DEVICE_MODE_POINTER) {
            for (BitSet32 idBits(mCurrentRawPointerData.touchingIdBits); !idBits.isEmpty(); ) {
                uint32_t id = idBits.clearFirstMarkedBit();
                const RawPointerData::Pointer& pointer = mCurrentRawPointerData.pointerForId(id);
                if (pointer.toolType == AMOTION_EVENT_TOOL_TYPE_STYLUS
                        || pointer.toolType == AMOTION_EVENT_TOOL_TYPE_ERASER) {
                    mCurrentStylusIdBits.markBit(id);
                } else if (pointer.toolType == AMOTION_EVENT_TOOL_TYPE_FINGER
                        || pointer.toolType == AMOTION_EVENT_TOOL_TYPE_UNKNOWN) {
                    mCurrentFingerIdBits.markBit(id);
                } else if (pointer.toolType == AMOTION_EVENT_TOOL_TYPE_MOUSE) {
                    mCurrentMouseIdBits.markBit(id);
                }
            }
            for (BitSet32 idBits(mCurrentRawPointerData.hoveringIdBits); !idBits.isEmpty(); ) {
                uint32_t id = idBits.clearFirstMarkedBit();
                const RawPointerData::Pointer& pointer = mCurrentRawPointerData.pointerForId(id);
                if (pointer.toolType == AMOTION_EVENT_TOOL_TYPE_STYLUS
                        || pointer.toolType == AMOTION_EVENT_TOOL_TYPE_ERASER) {
                    mCurrentStylusIdBits.markBit(id);
                }
            }

            // Stylus takes precedence over all tools, then mouse, then finger.
            PointerUsage pointerUsage = mPointerUsage;
            if (!mCurrentStylusIdBits.isEmpty()) {
                mCurrentMouseIdBits.clear();
                mCurrentFingerIdBits.clear();
                pointerUsage = POINTER_USAGE_STYLUS;
            } else if (!mCurrentMouseIdBits.isEmpty()) {
                mCurrentFingerIdBits.clear();
                pointerUsage = POINTER_USAGE_MOUSE;
            } else if (!mCurrentFingerIdBits.isEmpty() || isPointerDown(mCurrentButtonState)) {
                pointerUsage = POINTER_USAGE_GESTURES;
            }

            dispatchPointerUsage(when, policyFlags, pointerUsage);
        } else {
            if (mDeviceMode == DEVICE_MODE_DIRECT
                    && mConfig.showTouches && mPointerController != NULL) {
                mPointerController->setPresentation(PointerControllerInterface::PRESENTATION_SPOT);
                mPointerController->fade(PointerControllerInterface::TRANSITION_GRADUAL);

                mPointerController->setButtonState(mCurrentButtonState);
                mPointerController->setSpots(mCurrentCookedPointerData.pointerCoords,
                        mCurrentCookedPointerData.idToIndex,
                        mCurrentCookedPointerData.touchingIdBits);
            }

            dispatchHoverExit(when, policyFlags);
            dispatchTouches(when, policyFlags);
            dispatchHoverEnterAndMove(when, policyFlags);
        }

        // Synthesize key up from raw buttons if needed.
        synthesizeButtonKeys(getContext(), AKEY_EVENT_ACTION_UP, when, getDeviceId(), mSource,
                policyFlags, mLastButtonState, mCurrentButtonState);
    }

    // Copy current touch to last touch in preparation for the next cycle.
    mLastRawPointerData.copyFrom(mCurrentRawPointerData);
    mLastCookedPointerData.copyFrom(mCurrentCookedPointerData);
    mLastButtonState = mCurrentButtonState;
    mLastFingerIdBits = mCurrentFingerIdBits;
    mLastStylusIdBits = mCurrentStylusIdBits;
    mLastMouseIdBits = mCurrentMouseIdBits;

    // Clear some transient state.
    mCurrentRawVScroll = 0;
    mCurrentRawHScroll = 0;
}

void TouchInputMapper::timeoutExpired(nsecs_t when) {
    if (mDeviceMode == DEVICE_MODE_POINTER) {
        if (mPointerUsage == POINTER_USAGE_GESTURES) {
            dispatchPointerGestures(when, 0 /*policyFlags*/, true /*isTimeout*/);
        }
    }
}

bool TouchInputMapper::consumeRawTouches(nsecs_t when, uint32_t policyFlags) {
    // Check for release of a virtual key.
    if (mCurrentVirtualKey.down) {
        if (mCurrentRawPointerData.touchingIdBits.isEmpty()) {
            // Pointer went up while virtual key was down.
            mCurrentVirtualKey.down = false;
            if (!mCurrentVirtualKey.ignored) {
#if DEBUG_VIRTUAL_KEYS
                ALOGD("VirtualKeys: Generating key up: keyCode=%d, scanCode=%d",
                        mCurrentVirtualKey.keyCode, mCurrentVirtualKey.scanCode);
#endif
                dispatchVirtualKey(when, policyFlags,
                        AKEY_EVENT_ACTION_UP,
                        AKEY_EVENT_FLAG_FROM_SYSTEM | AKEY_EVENT_FLAG_VIRTUAL_HARD_KEY);
            }
            return true;
        }

        if (mCurrentRawPointerData.touchingIdBits.count() == 1) {
            uint32_t id = mCurrentRawPointerData.touchingIdBits.firstMarkedBit();
            const RawPointerData::Pointer& pointer = mCurrentRawPointerData.pointerForId(id);
            const VirtualKey* virtualKey = findVirtualKeyHit(pointer.x, pointer.y);
            if (virtualKey && virtualKey->keyCode == mCurrentVirtualKey.keyCode) {
                // Pointer is still within the space of the virtual key.
                return true;
            }
        }

        // Pointer left virtual key area or another pointer also went down.
        // Send key cancellation but do not consume the touch yet.
        // This is useful when the user swipes through from the virtual key area
        // into the main display surface.
        mCurrentVirtualKey.down = false;
        if (!mCurrentVirtualKey.ignored) {
#if DEBUG_VIRTUAL_KEYS
            ALOGD(
