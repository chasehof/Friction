#Friction
A high Frequency Telemetry Dashboard for process metrics and observability


## The Problem

Standard system monitors (top, htop, btop) provide a high level view but miss the micro friction that can kill performance in applications. Profiliers provide deep insight but can slow down the execution by orders of magnitude and are typically difficult to set up.

Friction fills the gap: a low overhead, real-time telemetry tool that monitors process specific churn.



Core Metrics:
1) Memory Velocity: Tracking allocations/deallocations per second to identify heap churn
2) Thread Contention: Monitoring context switches and mutex wait times
3) Syscall Frequency: Identifying noisy I/O paths that trap into the kernel too often
4) Data locality: Using PMCs to track L1/L2 cache misses
