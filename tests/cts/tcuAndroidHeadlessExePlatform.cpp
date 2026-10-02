/* Local build fix (not upstream CTS): a headless createPlatform() for
 * DEQP_ANDROID_EXE builds.
 *
 * The Android platform in framework/platform/android is built around a
 * NativeActivity and never defines createPlatform(), which tcuMain.cpp needs
 * when deqp-vk is linked as a plain executable. This provides one with no GL,
 * no EGL and no window system: Vulkan only, library loaded from
 * --deqp-vk-library-path (default libvulkan.so). Memory limits mirror
 * tcuAndroidPlatform.cpp. WSI tests report NotSupported.
 */
#include "tcuPlatform.hpp"
#include "tcuFunctionLibrary.hpp"
#include "vkPlatform.hpp"
#include "gluPlatform.hpp"
#include "egluPlatform.hpp"
#include "deUniquePtr.hpp"

#include <sys/utsname.h>
#include <unistd.h>
#include <cstring>

namespace
{

class HeadlessVulkanLibrary : public vk::Library
{
public:
    HeadlessVulkanLibrary(const char *path)
        : m_library(path != nullptr ? path : "libvulkan.so")
        , m_driver(m_library)
    {
    }
    const vk::PlatformInterface &getPlatformInterface(void) const
    {
        return m_driver;
    }
    const tcu::FunctionLibrary &getFunctionLibrary(void) const
    {
        return m_library;
    }

private:
    const tcu::DynamicFunctionLibrary m_library;
    const vk::PlatformDriver m_driver;
};

class HeadlessVulkanPlatform : public vk::Platform
{
public:
#ifdef DE_PLATFORM_USE_LIBRARY_TYPE
    vk::Library *createLibrary(LibraryType libraryType, const char *libraryPath) const
    {
        if (libraryType != LIBRARY_TYPE_VULKAN)
            TCU_THROW(InternalError, "Unknown library type requested");
        return new HeadlessVulkanLibrary(libraryPath);
    }
#else
    vk::Library *createLibrary(const char *libraryPath) const
    {
        return new HeadlessVulkanLibrary(libraryPath);
    }
#endif
    void describePlatform(std::ostream &dst) const
    {
        utsname u;
        std::memset(&u, 0, sizeof(u));
        uname(&u);
        dst << "OS: " << u.sysname << " " << u.release << " " << u.version << "\n";
        dst << "CPU: " << u.machine << "\n";
        dst << "Platform: headless Android executable (local build)\n";
    }
};

class HeadlessPlatform : public tcu::Platform
{
public:
    const vk::Platform &getVulkanPlatform(void) const
    {
        return m_vk;
    }
    void getMemoryLimits(tcu::PlatformMemoryLimits &limits) const
    {
        const size_t MiB  = (size_t)1 << 20;
        const size_t base = 400 * MiB;
        const long pages  = sysconf(_SC_PHYS_PAGES);
        const long psize  = sysconf(_SC_PAGESIZE);
        const int64_t total = (int64_t)pages * (int64_t)psize;
        const int64_t usable = (int64_t)((double)(total - (int64_t)base) * 0.25);
        limits.totalSystemMemory = usable > (int64_t)(16 * MiB) ? (size_t)usable : 16 * MiB;
        limits.totalDeviceLocalMemory            = 0;
        limits.deviceMemoryAllocationGranularity = 64 * 1024;
        limits.devicePageSize                    = 4096;
        limits.devicePageTableEntrySize          = 8;
        limits.devicePageTableHierarchyLevels    = 3;
    }

private:
    HeadlessVulkanPlatform m_vk;
};

} // namespace

tcu::Platform *createPlatform(void)
{
    return new HeadlessPlatform();
}
