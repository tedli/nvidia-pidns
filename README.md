# nvidia-pidns

非 host pid 模式下，容器内，`nvidia-smi` 显示容器内进程，nvidia 内核模块补丁。

## 背景

nvidia 闭源驱动，非 host pid 在容器内，nvidia-smi 显示不出容器内使用 gpu 的进程。
原因是`nvidia-smi`内逻辑调 ioctl 时，接口回的 pid 是宿主机 pid，在容器你进而
read `/proc/<pid>/cmdline` 获取进程名时失败所导致。

> `modinfo nvidia`可以查看是否是闭源，LICENSE 是 NVIDIA 则为闭源。

### [开源版 open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules)

英伟达有开源版驱动，开源版修复了此问题。但是只能显示进程，无法显示出进程使用的显存大小。

### 前辈

- https://github.com/gpues/nvidia-pidns
- https://github.com/gh2o/nvidia-pidns
- https://github.com/matpool/mpu

这几个仓库，是大佬在英伟达没开源出驱动之前，自己逆向，“猜”出的补丁，修复了此问题。致敬！:bow:
但是已经不更新维护，而且 linux 内核在这个 [commit](https://github.com/torvalds/linux/commit/f2824db1b49f947ba6e208ddf02edf4b1391480a#diff-58c56170305afda4af8cedf5db1cae8cbbe2b5ab3ea52afe2a8f6ca633ba2d78L2123) 已经去掉了 `d_instantate_anon` 接口，导致现有代码，在 6.8 内核后，无法编译通过。

## 编译

```bash
make -C /lib/modules/$(shell uname -r)/build M=$(pwd) modules
make -C /lib/modules/$(shell uname -r)/build M=$(pwd) clean
````

## 安装例

```yaml
# 脱密后未验证，仅作参考
apiVersion: apps/v1
kind: DaemonSet
metadata:
  name: nvidia-pidns-insmod
  labels:
    app: nvidia-pidns-insmod
spec:
  selector:
    matchLabels:
      app: nvidia-pidns-insmod
  updateStrategy:
    type: OnDelete
  template:
    metadata:
      labels:
        app: nvidia-pidns-insmod
    spec:
      containers:
        - name: insmod
          image: ubuntu:jammy-20240911.1
          command:
            - bash
          args:
            - -c
            - >
              if ! nsenter -t 1 -a lsmod | grep nvidia_pidns > /dev/null; then
              cp -Lrf /opt/nvidia-pidns/nvidia-pidns-$(uname -r).tar.xz /opt/host-tmp;
              nsenter -t 1 -a bash -c "tar -Jxf /tmp/nvidia-pidns-$(uname -r).tar.xz -C /;
              insmod /lib/modules/$(uname -r)/kernel/drivers/nvidia/nvidia-pidns.ko";
              fi;
              sleep infinity
          resources:
            requests:
              cpu: 200m
              memory: 256Mi
            limits:
              cpu: 200m
              memory: 256Mi
          securityContext:
            privileged: true
          volumeMounts:
            - name: nvidia-pidns
              mountPath: /opt/nvidia-pidns
              readOnly: true
            - name: tmp
              mountPath: /opt/host-tmp
      volumes:
        - name: nvidia-pidns
          configMap:
            name: nvidia-pidns-insmod
            defaultMode: 420
        - name: tmp
          hostPath:
            path: /tmp
            type: Directory
      hostNetwork: true
      hostPID: true
      hostIPC: true
      dnsPolicy: ClusterFirstWithHostNet
      tolerations:
        - operator: Exists
      automountServiceAccountToken: false
      priorityClassName: system-cluster-critical
---
apiVersion: v1
binaryData:
  nvidia-pidns-5.15.0-25-generic.tar.xz: 略
  nvidia-pidns-5.15.0-107-generic.tar.xz: 略
  nvidia-pidns-5.15.0-122-generic.tar.xz: 略
kind: ConfigMap
metadata:
  name: nvidia-pidns-insmod
  labels:
    app: nvidia-pidns-insmod
    component: modules
```