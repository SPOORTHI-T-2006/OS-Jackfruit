# 📦 OS-Jackfruit — Mini Container Runtime

---

# 1️⃣ Team Information

| Name                    | SRN              |
| ----------------------- | ---------------- |
| SPOORTHI T              | PES1UG24CS464


---

# 2️⃣ Build, Load, and Run Instructions

## 🔧 Step 1: Build the Project

```bash
cd boilerplate
make clean
make
```

This compiles:

* engine (CLI tool)
* workload binaries (memory\_hog, cpu\_hog, io\_pulse)
* kernel module (monitor.ko)

---

## 🔌 Step 2: Load Kernel Module

```bash
sudo insmod monitor.ko
```

Verify:

```bash
dmesg | tail
```

Check device:

```bash
ls -l /dev/container_monitor
```

---

## 🧠 Step 3: Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

---

## 📁 Step 4: Prepare Root Filesystems

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

## 🚀 Step 5: Start Containers

Open another terminal and run:

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
```

---

## 📊 Step 6: List Containers

```bash
sudo ./engine ps
```

---

## 📜 Step 7: View Logs

```bash
sudo ./engine logs alpha
```

---

## 🧪 Step 8: Run Workloads

Copy workload into container:

```bash
cp memory_hog ./rootfs-alpha/
```

Run:

```bash
sudo ./engine start test ./rootfs-alpha "./memory_hog"
```

---

## 📈 Step 9: Scheduling Experiment

```bash
sudo ./engine start fast ./rootfs-alpha "./cpu_hog" --nice -5
sudo ./engine start slow ./rootfs-alpha "./cpu_hog" --nice 10
```

---

## 🛑 Step 10: Stop Containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

---

## 📋 Step 11: Check Kernel Logs

```bash
dmesg | tail
```

---

## 🧹 Step 12: Cleanup

```bash
sudo pkill -f engine
sudo rm -f /tmp/mini_runtime.sock
sudo rmmod monitor
make clean
```

---

# 3️⃣ Demo with Screenshots



---

## 📸 Screenshot 1 — Multi-container Supervision

**Caption:** Two or more containers running under one supervisor process.

📷 *(Paste screenshot showing alpha and beta in running state)*
<img width="706" height="171" alt="image" src="https://github.com/user-attachments/assets/cf6736a9-12b5-4c6a-b12d-53593ec9236a" />

---

## 📸 Screenshot 2 — Metadata Tracking

**Caption:** Output of `engine ps` showing container metadata.

📷 *(Paste screenshot of ps output)*

---

## 📸 Screenshot 3 — Bounded-buffer Logging

**Caption:** Logs captured from container showing logging pipeline.

📷 *(Paste screenshot of engine logs output)*

---

## 📸 Screenshot 4 — CLI and IPC

**Caption:** CLI command interacting with supervisor via socket.

📷 *(Paste screenshot showing command + response)*

---

## 📸 Screenshot 5 — Soft-limit Warning

**Caption:** Kernel log showing soft memory limit warning.

📷 *(Paste screenshot of dmesg output with soft limit)*

---

## 📸 Screenshot 6 — Hard-limit Enforcement

**Caption:** Container killed after exceeding hard memory limit.

📷 *(Paste screenshot showing dmesg + ps exited state)*

---

## 📸 Screenshot 7 — Scheduling Experiment

**Caption:** CPU scheduling difference using nice values.

📷 *(Paste screenshot showing different nice values / CPU usage)*

---

## 📸 Screenshot 8 — Clean Teardown

**Caption:** No running containers after shutdown.

📷 *(Paste screenshot of ps aux showing no containers)*

---

# 4️⃣ Engineering Analysis

### Process Isolation

Containers are created using Linux process primitives like fork and separate root filesystems. This ensures each container runs independently.

---

### Inter-Process Communication (IPC)

Communication between CLI and supervisor uses a UNIX domain socket (`/tmp/mini_runtime.sock`), enabling command-response interaction.

---

### Resource Monitoring

A kernel module monitors memory usage of containers and enforces limits at kernel level.

---

### Logging Mechanism

A bounded-buffer producer-consumer model is used to safely handle logs between container output and logging threads.

---

### Scheduling Behavior

CPU scheduling differences are demonstrated using nice values to assign different priorities.

---

# 5️⃣ Design Decisions and Tradeoffs

### Namespace Isolation

* Choice: Root filesystem-based isolation
* Tradeoff: Less secure than full namespaces
* Reason: Simpler implementation

---

### Supervisor Architecture

* Choice: Single supervisor process
* Tradeoff: Single point of failure
* Reason: Easier coordination

---

### IPC and Logging

* Choice: UNIX socket + bounded buffer
* Tradeoff: Synchronization complexity
* Reason: Reliable communication

---

### Kernel Monitor

* Choice: Kernel module for monitoring
* Tradeoff: Requires root access
* Reason: Accurate enforcement

---

### Scheduling

* Choice: nice-based control
* Tradeoff: Limited flexibility
* Reason: Easy demonstration of scheduling

---

# 6️⃣ Scheduler Experiment Results

### Observations

| Container | Nice Value | Behavior         |
| --------- | ---------- | ---------------- |
| fast      | -5         | Higher CPU usage |
| slow      | 10         | Lower CPU usage  |

---

### Result

Lower nice value processes get higher CPU priority, while higher nice value processes receive less CPU time.

---

### Conclusion

This demonstrates how the Linux scheduler prioritizes processes based on nice values.

---

# 📌 Final Summary

This project demonstrates:

* Multi-container supervision
* IPC using sockets
* Kernel-level resource control
* Logging using synchronization
* CPU scheduling behavior

---

# 🚨 Before Submission

* Add all screenshots
* Fill SRN details
* Verify GitHub repo
