// Original Cell Framework implementation; see LICENSE.
#include <cell/graph_project.hpp>
#include <jni.h>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

std::string readPath(JNIEnv& env, jbyteArray bytes) {
    if (!bytes || env.GetArrayLength(bytes) == 0)
        throw std::invalid_argument("Expected a filesystem path");
    std::string path(static_cast<std::size_t>(env.GetArrayLength(bytes)), '\0');
    env.GetByteArrayRegion(bytes, 0, static_cast<jsize>(path.size()),
                           reinterpret_cast<jbyte*>(path.data()));
    if (env.ExceptionCheck())
        return {};
    if (path.find('\0') != std::string::npos)
        throw std::invalid_argument("A filesystem path cannot contain NUL");
    return path;
}

jbyteArray errorBytes(JNIEnv& env, const char* message) {
    const auto length = static_cast<jsize>(std::strlen(message));
    const auto result = env.NewByteArray(length);
    if (result)
        env.SetByteArrayRegion(result, 0, length, reinterpret_cast<const jbyte*>(message));
    return result;
}

} // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_cell_examples_GraphRenderer_renderNative(JNIEnv* env, jclass, jbyteArray project,
                                                  jbyteArray destination) noexcept {
    try {
        const auto sourcePath = readPath(*env, project);
        if (env->ExceptionCheck())
            return nullptr;
        const auto destinationPath = readPath(*env, destination);
        if (env->ExceptionCheck())
            return nullptr;
        cell::graph::renderProject(cell::graph::openProject(cell::File(sourcePath)),
                                   cell::File(destinationPath));
        return nullptr;
    } catch (const std::exception& error) {
        return errorBytes(*env, error.what());
    } catch (...) {
        return errorBytes(*env, "Unexpected error rendering Cell graph project");
    }
}
