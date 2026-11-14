# The JNI glue (bridge/jni/loomora_jni.cpp) exports Java_fr_bansan_loomora_probe_Native_*: the class and its
# native methods keep their names.
-keep class fr.bansan.loomora.probe.Native {
    native <methods>;
}
