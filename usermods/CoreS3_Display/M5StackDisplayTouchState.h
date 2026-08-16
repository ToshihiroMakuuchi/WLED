#pragma once

#include <stdint.h>

// Common M5Stack controller Touch state types.
//
// These types contain no hardware access and no WLED operations.
// Touch geometry is defined separately in M5StackDisplayUI.h.
// The runtime state machine remains in CoreS3_Display.cpp for now.

// Touch action selected when a press begins.
enum M5StackTouchTarget : uint8_t {
  M5STACK_TOUCH_TARGET_NONE = 0,

  M5STACK_TOUCH_TARGET_POWER,

  M5STACK_TOUCH_TARGET_BRIGHTNESS_DOWN, M5STACK_TOUCH_TARGET_BRIGHTNESS_UP,

  M5STACK_TOUCH_TARGET_EFFECT_PREV, M5STACK_TOUCH_TARGET_EFFECT_DETAIL, M5STACK_TOUCH_TARGET_EFFECT_NEXT,

  M5STACK_TOUCH_TARGET_COLOR_OPEN, M5STACK_TOUCH_TARGET_PRESET_OPEN,

  M5STACK_TOUCH_TARGET_BACK,

  M5STACK_TOUCH_TARGET_HUE_DOWN, M5STACK_TOUCH_TARGET_HUE_UP,

  M5STACK_TOUCH_TARGET_SATURATION_DOWN, M5STACK_TOUCH_TARGET_SATURATION_UP,

  M5STACK_TOUCH_TARGET_SPEED_DOWN, M5STACK_TOUCH_TARGET_SPEED_UP,

  M5STACK_TOUCH_TARGET_INTENSITY_DOWN, M5STACK_TOUCH_TARGET_INTENSITY_UP,

  M5STACK_TOUCH_TARGET_PALETTE_PREV, M5STACK_TOUCH_TARGET_PALETTE_NEXT,

  M5STACK_TOUCH_TARGET_PRESET_PREV, M5STACK_TOUCH_TARGET_PRESET_NEXT,

  M5STACK_TOUCH_TARGET_PRESET_MANAGE, M5STACK_TOUCH_TARGET_PRESET_SAVE_NEW, M5STACK_TOUCH_TARGET_PRESET_SAVE_HOLD,

  M5STACK_TOUCH_TARGET_PRESET_OVERWRITE_OPEN, M5STACK_TOUCH_TARGET_PRESET_OVERWRITE_PREV, M5STACK_TOUCH_TARGET_PRESET_OVERWRITE_NEXT, M5STACK_TOUCH_TARGET_PRESET_OVERWRITE_HOLD,

  M5STACK_TOUCH_TARGET_PRESET_DELETE_OPEN, M5STACK_TOUCH_TARGET_PRESET_DELETE_PREV, M5STACK_TOUCH_TARGET_PRESET_DELETE_NEXT, M5STACK_TOUCH_TARGET_PRESET_DELETE_HOLD,

  M5STACK_TOUCH_TARGET_PRESET_BOOT_OPEN, M5STACK_TOUCH_TARGET_PRESET_BOOT_PREV, M5STACK_TOUCH_TARGET_PRESET_BOOT_NEXT, M5STACK_TOUCH_TARGET_PRESET_BOOT_HOLD };

// One hit-test snapshot built from the current Touch coordinates.
struct M5StackTouchHitState {
  bool insidePower = false;
  bool insideBrightnessDown = false;
  bool insideBrightnessUp = false;
  bool insideEffectPrev = false;
  bool insideEffectDetail = false;
  bool insideEffectNext = false;
  bool insideColor = false;
  bool insidePresetOpen = false;
  bool insideBack = false;
  bool insideHueDown = false;
  bool insideHueUp = false;
  bool insideSaturationDown = false;
  bool insideSaturationUp = false;
  bool insideSpeedDown = false;
  bool insideSpeedUp = false;
  bool insideIntensityDown = false;
  bool insideIntensityUp = false;
  bool insidePalettePrev = false;
  bool insidePaletteNext = false;
  bool insidePresetPrev = false;
  bool insidePresetNext = false;
  bool insidePresetManage = false;
  bool insidePresetSaveNew = false;
  bool insidePresetSaveHold = false;
  bool insidePresetOverwriteOpen = false;
  bool insidePresetOverwritePrev = false;
  bool insidePresetOverwriteNext = false;
  bool insidePresetOverwriteHold = false;
  bool insidePresetDeleteOpen = false;
  bool insidePresetDeletePrev = false;
  bool insidePresetDeleteNext = false;
  bool insidePresetDeleteHold = false;
  bool insidePresetBootOpen = false;
  bool insidePresetBootPrev = false;
  bool insidePresetBootNext = false;
  bool insidePresetBootHold = false;
};

// Shared long-press / repeat timing state.
struct M5StackRepeatTouchState {
  unsigned long pressStart = 0;
  unsigned long lastRepeat = 0;
  bool longPressActive = false;
};

// Consolidated runtime state for one active Touch interaction.
//
// Field names/defaults intentionally match the previously separate
// CoreS3_Display.cpp members so this is a storage-only refactor.
struct M5StackTouchRuntimeState {
  unsigned long lastTouchPoll = 0;
  unsigned long lastTouchAction = 0;
  unsigned long touchReleaseCandidate = 0;
  M5StackTouchTarget touchTarget = M5STACK_TOUCH_TARGET_NONE;
  bool touchActive = false;
  bool lastTouchInsidePower = false;
  bool lastTouchInsideBrightness = false;
  bool lastTouchInsideEffect = false;
  bool lastTouchInsideEffectDetail = false;
  bool lastTouchInsideColor = false;
  bool lastTouchInsidePresetOpen = false;
  bool lastTouchInsideBack = false;
  bool lastTouchInsideHue = false;
  bool lastTouchInsideSaturation = false;
  bool lastTouchInsideSpeed = false;
  bool lastTouchInsideIntensity = false;
  bool lastTouchInsidePalette = false;
  bool lastTouchInsidePresetNav = false;
  bool lastTouchInsidePresetManage = false;
  bool lastTouchInsidePresetSaveNew = false;
  bool lastTouchInsidePresetSaveHold = false;
  bool lastTouchInsidePresetOverwriteOpen = false;
  bool lastTouchInsidePresetOverwriteNav = false;
  bool lastTouchInsidePresetOverwriteHold = false;
  bool lastTouchInsidePresetDeleteOpen = false;
  bool lastTouchInsidePresetDeleteNav = false;
  bool lastTouchInsidePresetDeleteHold = false;
  bool lastTouchInsidePresetBootOpen = false;
  bool lastTouchInsidePresetBootNav = false;
  bool lastTouchInsidePresetBootHold = false;
  M5StackRepeatTouchState brightnessRepeatState;
  M5StackRepeatTouchState effectRepeatState;
  M5StackRepeatTouchState hueRepeatState;
  M5StackRepeatTouchState saturationRepeatState;
  M5StackRepeatTouchState speedRepeatState;
  M5StackRepeatTouchState intensityRepeatState;
  M5StackRepeatTouchState paletteRepeatState;
  M5StackRepeatTouchState presetRepeatState;
  int16_t lastTouchX = -1;
  int16_t lastTouchY = -1;
};
