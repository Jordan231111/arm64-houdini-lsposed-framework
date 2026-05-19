#pragma once

#include <jni.h>

namespace native_utils {

/**
 * Registers the NativeUtils JNI surface on com.template.lsposed.NativeUtils.
 * Called from template_native.cpp's JNI_OnLoad. Returns true on success.
 */
bool register_natives(JNIEnv *env);

}  // namespace native_utils
