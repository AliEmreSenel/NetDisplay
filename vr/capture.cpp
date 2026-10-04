// SPDX-License-Identifier: GPL-2.0-only
#include "capture.h"
#include "protocol.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
#include <chrono>
#include <vulkan/vulkan.h>

namespace {
void check(VkResult r) {
  if (r != VK_SUCCESS)
    throw std::runtime_error("Vulkan error " + std::to_string(r));
}
void exact(int fd, void *out, size_t n) {
  auto p = static_cast<char *>(out);
  while (n) {
    ssize_t r = recv(fd, p, n, 0);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      throw std::runtime_error("compositor IPC disconnected");
    p += r;
    n -= r;
  }
}
} // namespace
struct ndvr_capture {
  int listener = -1, socket = -1;
  std::string path;
  unsigned width = 0, height = 0;
  VkInstance instance{};
  VkDevice device{};
  VkPhysicalDevice gpu{};
  VkQueue queue{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  VkBuffer staging{};
  VkDeviceMemory staging_memory{};
  void *mapped = nullptr;
  VkImage images[3]{};
  VkDeviceMemory memory[3]{};
  VkSemaphore semaphore[3]{};
  bool first[3]{true, true, true};
  bool bgra = false;
  uint64_t readback_ns = 0;
  int fds[6]{-1, -1, -1, -1, -1, -1};
  ~ndvr_capture() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (mapped)
        vkUnmapMemory(device, staging_memory);
      if (staging)
        vkDestroyBuffer(device, staging, nullptr);
      if (staging_memory)
        vkFreeMemory(device, staging_memory, nullptr);
      if (fence)
        vkDestroyFence(device, fence, nullptr);
      if (pool)
        vkDestroyCommandPool(device, pool, nullptr);
      for (int i = 0; i < 3; i++) {
        if (semaphore[i])
          vkDestroySemaphore(device, semaphore[i], nullptr);
        if (images[i])
          vkDestroyImage(device, images[i], nullptr);
        if (memory[i])
          vkFreeMemory(device, memory[i], nullptr);
      }
      vkDestroyDevice(device, nullptr);
    }
    if (instance)
      vkDestroyInstance(instance, nullptr);
    for (int fd : fds)
      if (fd >= 0)
        close(fd);
    if (socket >= 0)
      close(socket);
    if (listener >= 0) {
      close(listener);
      unlink(path.c_str());
    }
  }
  void connect_gpu() {
    socket = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (socket < 0)
      throw std::runtime_error("accept compositor");
    ucred cred{};
    socklen_t len = sizeof(cred);
    if (getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &cred, &len) ||
        cred.uid != getuid())
      throw std::runtime_error("unexpected compositor user");
    timeval timeout{2, 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    init_packet init{};
    exact(socket, &init, sizeof(init));
    auto &info = init.image_create_info;
    if (init.num_images != 3 || info.extent.width != width ||
        info.extent.height != height || info.extent.depth != 1 ||
        info.mipLevels != 1 || info.arrayLayers != 1 ||
        info.samples != VK_SAMPLE_COUNT_1_BIT ||
        info.imageType != VK_IMAGE_TYPE_2D ||
        info.tiling != VK_IMAGE_TILING_OPTIMAL ||
        info.sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        info.queueFamilyIndexCount || info.pNext || info.pQueueFamilyIndices)
      throw std::runtime_error("invalid compositor image configuration; "
                               "restart SteamVR with matching mode");
    bgra = info.format == VK_FORMAT_B8G8R8A8_UNORM ||
           info.format == VK_FORMAT_B8G8R8A8_SRGB;
    if (!bgra && info.format != VK_FORMAT_R8G8B8A8_UNORM &&
        info.format != VK_FORMAT_R8G8B8A8_SRGB)
      throw std::runtime_error("unsupported compositor pixel format");
    char data;
    iovec iov{&data, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(fds))]{};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != 1)
      throw std::runtime_error("missing compositor image descriptors");
    auto c = CMSG_FIRSTHDR(&message);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
        c->cmsg_len != CMSG_LEN(sizeof(fds)) || message.msg_flags & MSG_CTRUNC)
      throw std::runtime_error("invalid compositor descriptors");
    memcpy(fds, CMSG_DATA(c), sizeof(fds));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "NetDisplay VR";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ic.pApplicationInfo = &app;
    check(vkCreateInstance(&ic, nullptr, &instance));
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    for (auto d : devices) {
      VkPhysicalDeviceIDProperties id{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 props{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      props.pNext = &id;
      vkGetPhysicalDeviceProperties2(d, &props);
      if (!memcmp(id.deviceUUID, init.device_uuid.data(), VK_UUID_SIZE))
        gpu = d;
    }
    if (!gpu)
      throw std::runtime_error("compositor GPU unavailable to VR backend");
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
    uint32_t family = 0;
    while (family < count &&
           !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      family++;
    if (family == count)
      throw std::runtime_error("no graphics queue");
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    const char *extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &timeline;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 2;
    di.ppEnabledExtensionNames = extensions;
    check(vkCreateDevice(gpu, &di, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    auto importSemaphore = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR"));
    if (!importSemaphore)
      throw std::runtime_error("missing external semaphore support");
    for (int i = 0; i < 3; i++) {
      VkExternalMemoryImageCreateInfo external{
          VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      info.pNext = &external;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      check(vkCreateImage(device, &info, nullptr, &images[i]));
      VkMemoryRequirements req;
      vkGetImageMemoryRequirements(device, images[i], &req);
      if (init.mem_index >= 32 ||
          !(req.memoryTypeBits & (1u << init.mem_index)))
        throw std::runtime_error("incompatible GPU memory type");
      VkMemoryDedicatedAllocateInfo dedicated{
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
      dedicated.image = images[i];
      VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
      imp.pNext = &dedicated;
      imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      imp.fd = fds[i * 2];
      VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc.pNext = &imp;
      alloc.allocationSize = req.size;
      alloc.memoryTypeIndex = init.mem_index;
      check(vkAllocateMemory(device, &alloc, nullptr, &memory[i]));
      fds[i * 2] = -1;
      check(vkBindImageMemory(device, images[i], memory[i], 0));
      VkSemaphoreTypeCreateInfo type{
          VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
      type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
      VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      si.pNext = &type;
      check(vkCreateSemaphore(device, &si, nullptr, &semaphore[i]));
      VkImportSemaphoreFdInfoKHR sem{
          VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
      sem.semaphore = semaphore[i];
      sem.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
      sem.fd = fds[i * 2 + 1];
      check(importSemaphore(device, &sem));
      fds[i * 2 + 1] = -1;
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = uint64_t(width) * height * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(vkCreateBuffer(device, &bi, nullptr, &staging));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, staging, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    uint32_t index = 0;
    for (; index < mp.memoryTypeCount; index++)
      if ((req.memoryTypeBits & (1u << index)) &&
          (mp.memoryTypes[index].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        break;
    if (index == mp.memoryTypeCount)
      throw std::runtime_error("no coherent readback memory");
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = req.size;
    ma.memoryTypeIndex = index;
    check(vkAllocateMemory(device, &ma, nullptr, &staging_memory));
    check(vkBindBufferMemory(device, staging, staging_memory, 0));
    check(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(device, &pi, nullptr, &pool));
    VkCommandBufferAllocateInfo ca{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &ca, &command));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device, &fi, nullptr, &fence));
    fprintf(stderr,
            "VR compositor attached: %ux%u %s, GPU shared images imported\n",
            width, height, bgra ? "BGRA" : "RGBA");
  }
  void copy(const present_packet &p) {
    if (p.image >= 3 || !p.semaphore_value)
      throw std::runtime_error("invalid present packet");
    check(vkResetCommandBuffer(command, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout =
        first[p.image] ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[p.image];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(command, images[p.image],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
                           &region);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                         nullptr);
    check(vkEndCommandBuffer(command));
    VkTimelineSemaphoreSubmitInfo ti{
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = 1;
    ti.pWaitSemaphoreValues = &p.semaphore_value;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &ti;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &semaphore[p.image];
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(vkResetFences(device, 1, &fence));
    check(vkQueueSubmit(queue, 1, &submit, fence));
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ULL));
    first[p.image] = false;
  }
};
extern "C" ndvr_capture *ndvr_capture_open(const char *path, unsigned width,
                                           unsigned height) {
  auto c = new ndvr_capture;
  c->path = path;
  c->width = width;
  c->height = height;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr.sun_path)) {
    delete c;
    return nullptr;
  }
  strcpy(addr.sun_path, path);
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  // Refuse to unlink an existing endpoint owned by another server.
  if (fd < 0 ||
      bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    perror("VR capture bind");
    if (fd >= 0)
      close(fd);
    delete c;
    return nullptr;
  }
  c->listener = fd;
  chmod(path, 0600);
  if (listen(fd, 1) < 0) {
    delete c;
    return nullptr;
  }
  return c;
}
extern "C" int ndvr_capture_next(ndvr_capture *c, const uint8_t **pixels,
                                 int *bgra, int timeout) {
  try {
    pollfd p{c->socket < 0 ? c->listener : c->socket, POLLIN, 0};
    int r = poll(&p, 1, timeout);
    if (r == 0 || (r < 0 && errno == EINTR))
      return 0;
    if (r < 0 || p.revents & (POLLERR | POLLHUP | POLLNVAL))
      throw std::runtime_error("compositor disconnected");
    if (c->socket < 0) {
      c->connect_gpu();
      return 0;
    }
    present_packet frame{};
    exact(c->socket, &frame, sizeof(frame));
    auto started = std::chrono::steady_clock::now();
    c->copy(frame);
    c->readback_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    unsigned char ack = 1;
    if (send(c->socket, &ack, 1, MSG_NOSIGNAL) != 1)
      throw std::runtime_error("compositor acknowledgement failed");
    *pixels = static_cast<const uint8_t *>(c->mapped);
    *bgra = c->bgra;
    return 1;
  } catch (const std::exception &e) {
    fprintf(stderr, "VR capture: %s\n", e.what());
    // Preserve the listening endpoint across SteamVR restarts and swapchain
    // recreation. Destroy imported GPU resources before accepting new images.
    int listener = c->listener;
    std::string path = c->path;
    unsigned width = c->width, height = c->height;
    c->listener = -1;
    c->~ndvr_capture();
    new (c) ndvr_capture;
    c->listener = listener;
    c->path = path;
    c->width = width;
    c->height = height;
    return 0;
  }
}
extern "C" void ndvr_capture_close(ndvr_capture *c) { delete c; }

extern "C" uint64_t ndvr_capture_readback_ns(const ndvr_capture *c) { return c->readback_ns; }
