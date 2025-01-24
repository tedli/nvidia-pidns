#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/printk.h>
#include <linux/pseudo_fs.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/uaccess.h>

typedef long (*IoControlHandler)(struct file *, unsigned int, unsigned long);

static IoControlHandler original_unlocked_ioctl = NULL;
static IoControlHandler original_compat_ioctl = NULL;

static struct file *nvidia_control_device = NULL;

static int InitializeDummyFsContext(struct fs_context *context) {
  return init_pseudo(context, 0xd09858b3) ? 0 : -ENOMEM;
}

static struct file_system_type dummy_fs_type = {
    .owner = THIS_MODULE,
    .name = "nvidia_pidns",
    .init_fs_context = InitializeDummyFsContext,
    .kill_sb = kill_anon_super,
};

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/kernel-open/common/inc/nv-chardev-numbers.h#L29
#define NV_MAJOR_DEVICE_NUMBER 195

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/kernel-open/common/inc/nv-chardev-numbers.h#L40
#define NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE 255

static const u32 kNvidiaControlDevice =
    MKDEV(NV_MAJOR_DEVICE_NUMBER, NV_MINOR_DEVICE_NUMBER_CONTROL_DEVICE);

static inline struct file *FindNvidiaControlDevice(void) {

  struct vfsmount *mount = kern_mount(&dummy_fs_type);
  if (IS_ERR(mount))
    return ERR_CAST(mount);

  struct inode *inode = alloc_anon_inode(mount->mnt_sb);
  if (IS_ERR(inode)) {
    kern_unmount(mount);
    return ERR_CAST(inode);
  }
  init_special_inode(inode, S_IFCHR | 0666, kNvidiaControlDevice);

  struct dentry *dentry = d_obtain_alias(inode);
  if (dentry == NULL) {
    iput(inode);
    kern_unmount(mount);
    return ERR_PTR(-ENOMEM);
  }

  struct path path = {.mnt = mount, .dentry = dentry};
  struct file *file = dentry_open(&path, O_RDWR, current_cred());

  if (IS_ERR(file))
    pr_err("nvidia-pidns: failed to open nvidiactl (%ld), is the nvidia module "
           "loaded?\n",
           PTR_ERR(file));

  dput(dentry);
  kern_unmount(mount);

  return file;
}

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/common/sdk/nvidia/inc/nvos.h#L2220-L2229
typedef struct {
  u32 client;
  u32 object;
  u32 method_id;
  u32 flags;
  void *parameter __attribute__((aligned(8)));
  u32 parameter_size;
  u32 status;
} InvocationContext;

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h#L3442
#define NV2080_CTRL_GPU_GET_PIDS_MAX_COUNT 950U

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h#L3446-L3451
typedef struct {
  u32 type;
  u32 id;
  u32 count;
  u32 pids[NV2080_CTRL_GPU_GET_PIDS_MAX_COUNT];
} GpuGetPidsParameter;

static inline long WrapGetGpuPids(struct file *file, unsigned int command,
                                  unsigned long arg, InvocationContext *context,
                                  IoControlHandler original) {

  long error = original(file, command, arg);
  if (error != 0)
    return error;

  GpuGetPidsParameter *parameter =
      kmalloc(sizeof(GpuGetPidsParameter), GFP_KERNEL);
  if (parameter == NULL)
    return -ENOMEM;

  if ((error = copy_from_user(parameter, context->parameter,
                              sizeof(GpuGetPidsParameter))) != 0) {
    kfree(parameter);
    return error;
  }

  int wrote = 0;

  rcu_read_lock();
  for (int i = 0; i < parameter->count; ++i) {
    struct pid *pid = find_pid_ns(parameter->pids[i], &init_pid_ns);
    if (pid != NULL) {
      pid_t pid_ns = pid_vnr(pid);
      if (pid_ns != 0)
        parameter->pids[wrote++] = pid_ns;
    }
  }
  rcu_read_unlock();

  for (int i = wrote; i < parameter->count; ++i)
    parameter->pids[i] = 0;

  parameter->count = wrote;

  if ((error = copy_to_user(context->parameter, parameter,
                            sizeof(GpuGetPidsParameter))) != 0) {
    kfree(parameter);
    return error;
  }

  kfree(parameter);

  return 0;
}

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h#L3499-L3506
typedef struct {
  u64 private __attribute__((aligned(8)));
  u64 owned __attribute__((aligned(8)));
  u64 duped __attribute__((aligned(8)));
  u64 protected_private __attribute__((aligned(8)));
  u64 protected_owned __attribute__((aligned(8)));
  u64 protected_duped __attribute__((aligned(8)));
} GpuPidInfoVideoMemoryUsageData;

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h#L3512-L3514
typedef union {
  GpuPidInfoVideoMemoryUsageData usage __attribute__((aligned(8)));
} GpuPidInfoData;

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h#L3472-L3475
typedef struct {
  u32 compute_instance_id;
  u32 gpu_instance_id;
} SmcSubscriptionInfo;

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h#L3554-L3560
typedef struct {
  u32 pid;
  u32 index;
  u32 result;
  GpuPidInfoData data __attribute__((aligned(8)));
  SmcSubscriptionInfo subscription;
} GpuPidInfo;

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h#L3584
#define NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT 200U

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h#L3588-L3591
typedef struct {
  u32 count;
  GpuPidInfo list[NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT]
      __attribute__((aligned(8)));
} GpuGetPidInfoParameter;

static inline long WrapGetGpuPidInfo(struct file *file, unsigned int command,
                                     unsigned long arg,
                                     InvocationContext *context,
                                     IoControlHandler original) {

  GpuGetPidInfoParameter *parameter =
      kmalloc(sizeof(GpuGetPidInfoParameter), GFP_KERNEL);
  if (parameter == NULL)
    return -ENOMEM;

  long error = copy_from_user(parameter, context->parameter,
                              sizeof(GpuGetPidInfoParameter));
  if (error != 0) {
    kfree(parameter);
    return error;
  }

  u32 *pids = kmalloc(parameter->count * sizeof(u32), GFP_KERNEL);
  if (pids == NULL) {
    kfree(parameter);
    return -ENOMEM;
  }

  for (int i = 0; i < parameter->count; ++i) {
    pids[i] = parameter->list[i].pid;
  }

  rcu_read_lock();
  for (int i = 0; i < parameter->count; ++i) {
    struct pid *pid = find_vpid(pids[i]);
    if (pid != NULL) {
      pid_t pid_host = pid_nr(pid);
      if (pid_host != 0)
        parameter->list[i].pid = pid_host;
    }
  }
  rcu_read_unlock();

  if ((error = copy_to_user(context->parameter, parameter,
                            sizeof(GpuGetPidInfoParameter))) != 0) {
    kfree(parameter);
    kfree(pids);
    return error;
  }

  if ((error = original(file, command, arg)) != 0) {
    kfree(parameter);
    kfree(pids);
    return error;
  }

  if ((error = copy_from_user(parameter, context->parameter,
                              sizeof(GpuGetPidInfoParameter))) != 0) {
    kfree(parameter);
    kfree(pids);
    return error;
  }

  for (int i = 0; i < parameter->count; ++i)
    parameter->list[i].pid = pids[i];
  kfree(pids);

  if ((error = copy_to_user(context->parameter, parameter,
                            sizeof(GpuGetPidInfoParameter))) != 0) {
    kfree(parameter);
    return error;
  }

  kfree(parameter);

  return 0;
}

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/kernel-open/common/inc/nv-ioctl-numbers.h#L29
#define NV_IOCTL_MAGIC 'F'
// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/nvidia/arch/nvalloc/unix/include/nv_escape.h#L30
#define NV_ESC_RM_CONTROL 0x2A

static const unsigned int kIoControlResourceManagerCommand =
    _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, NV_ESC_RM_CONTROL,
         sizeof(InvocationContext));

// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/nvidia/generated/g_subdevice_nvoc.c#L1103
static const u32 kGpuGetPidsMethodId = 0x2080018du;
// https://github.com/NVIDIA/open-gpu-kernel-modules/blob/9d0b0414a5304c3679c5db9d44d2afba8e58cc1b/src/nvidia/generated/g_subdevice_nvoc.c#L1118
static const u32 kGpuGetPidInfoMethodId = 0x2080018eu;

static inline long WrapPidNs(struct file *file, unsigned int command,
                             unsigned long arg, IoControlHandler original) {

  if (file_inode(file)->i_rdev != kNvidiaControlDevice)
    return original(file, command, arg);

  if (command != kIoControlResourceManagerCommand)
    return original(file, command, arg);

  InvocationContext context = {0};
  long error =
      copy_from_user(&context, (void __user *)arg, sizeof(InvocationContext));
  if (error != 0)
    return error;

  switch (context.method_id) {
  case kGpuGetPidsMethodId:
    error = WrapGetGpuPids(file, command, arg, &context, original);
    break;
  case kGpuGetPidInfoMethodId:
    error = WrapGetGpuPidInfo(file, command, arg, &context, original);
    break;
  default:
    error = original(file, command, arg);
    break;
  }

  return error;
}

static long WrapPidNsUnlocked(struct file *file, unsigned int command,
                              unsigned long arg) {
  return WrapPidNs(file, command, arg, original_unlocked_ioctl);
}

static long WrapPidNsCompat(struct file *file, unsigned int command,
                            unsigned long arg) {
  return WrapPidNs(file, command, arg, original_compat_ioctl);
}

static int __init OnInitialize(void) {

  struct file *file = FindNvidiaControlDevice();

  if (IS_ERR(file))
    return PTR_ERR(file);

  nvidia_control_device = file;
  struct file_operations *operations = (struct file_operations *)file->f_op;

  original_unlocked_ioctl = operations->unlocked_ioctl;
  original_compat_ioctl = operations->compat_ioctl;

  wmb();
  operations->unlocked_ioctl = WrapPidNsUnlocked;
  operations->compat_ioctl = WrapPidNsCompat;

  printk(KERN_INFO "nvidia_pidns loaded\n");
  return 0;
}

static void __exit OnCleanup(void) {

  if (nvidia_control_device == NULL)
    return;

  struct file *file = nvidia_control_device;
  struct file_operations *fops = (struct file_operations *)file->f_op;

  if (original_unlocked_ioctl != NULL)
    fops->unlocked_ioctl = original_unlocked_ioctl;
  if (original_compat_ioctl != NULL)
    fops->compat_ioctl = original_compat_ioctl;

  wmb();
  fput(nvidia_control_device);

  printk(KERN_INFO "nvidia_pidns unloaded\n");
}

module_init(OnInitialize);
module_exit(OnCleanup);

MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: nvidia");
