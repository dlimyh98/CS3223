# Testcase 11

1. Observe `testcase11.c`, in particular note *Blk 1*
2. *Blk 1* is read&pinned once in line 1;
3. *Blk 1* is read&pinned once more in line 20;
4. Question is should *Blk 1* be considered to have reference count of 1 or 2?
    4a. If it is being read&pinned by separate processes, then it should have reference count 2
    4b. If it is being read&pinned by the same process, then it should have reference count 1
5. Consult `results-11.txt`, the results of running `testcase11.c`
5a. Look at the warnings that are at the bottom of the file...
```
WARNING:  buffer refcount leak: [016] (rel=base/73992/73993, blockNum=1, flags=0x83000000, refcount=1 2)
WARNING:  buffer refcount leak: [014] (rel=base/73992/73993, blockNum=17, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [001] (rel=base/73992/73993, blockNum=16, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [002] (rel=base/73992/73993, blockNum=15, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [003] (rel=base/73992/73993, blockNum=14, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [004] (rel=base/73992/73993, blockNum=13, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [005] (rel=base/73992/73993, blockNum=12, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [006] (rel=base/73992/73993, blockNum=11, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [007] (rel=base/73992/73993, blockNum=10, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [008] (rel=base/73992/73993, blockNum=9, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [009] (rel=base/73992/73993, blockNum=8, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [010] (rel=base/73992/73993, blockNum=7, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [011] (rel=base/73992/73993, blockNum=6, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [012] (rel=base/73992/73993, blockNum=5, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [013] (rel=base/73992/73993, blockNum=4, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [016] (rel=base/73992/73993, blockNum=1, flags=0x83000000, refcount=1 1)
WARNING:  buffer refcount leak: [015] (rel=base/73992/73993, blockNum=2, flags=0x83000000, refcount=1 1)
```
   5b. From GPT
```
In PostgreSQL, the warning message "WARNING: buffer refcount leak: [016] (rel=base/73956/73957, blockNum=1, flags=0x83000000, refcount=1 2)" indicates a potential issue related to buffer management.  
  
Let's break down the components of the warning message:  
  
- "WARNING: buffer refcount leak": This indicates that there is a potential problem with the reference count of a buffer.  
- "[016]": This is the process ID (PID) of the PostgreSQL backend that generated the warning.  
- "(rel=base/73956/73957, blockNum=1, flags=0x83000000, refcount=1 2)": This provides additional information about the buffer in question:  
- "rel=base/73956/73957": This specifies the relation or table file where the buffer is located. The numbers represent the database OID, tablespace OID, and relation OID, respectively.  
- "blockNum=1": This indicates the block number within the relation where the buffer is located.  
- "flags=0x83000000": These are hexadecimal flags that provide further details about the buffer's state.  
- "refcount=1 2": This shows the current reference count of the buffer. The first number represents the expected reference count, while the second number represents the actual reference count.  
  
The warning suggests that there may be a discrepancy between the expected and actual reference counts of the buffer, which could potentially lead to a memory leak or other issues. It is advisable to investigate and address this warning to ensure proper buffer management and system stability.
```
5c. So Chee Yong's implementation seems to suggest that read&pin is from separate processes, but Postgre's implementation seems to suggest that read&pin is from the same process.


6. Which approach should we go with?