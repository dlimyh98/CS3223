Suppose we want to add a custom `testcase10.c`

1. Navigate to `.../cs3223-assign1/test_bufmgr/testcases` **directory**
2. Create your `testcase10.c` in this directory (with the appropriate logic)
3. Navigate to `.../cs3223-assign1/test_bufmgr.c` **file**. In the `test_bufmgr(...)` function, add an extra case to the switch statement corresponding to `testcase10.c`
4. Navigate to `cs3223-assign1`, and do `./updateTest.sh` to reinstall *test_bufmgr* extension with our new testcase
5. In the **same directory**, edit `for testno in  {...}` under `test-lru.sh` to the appropriate range
6. Do Chee Yong's commands (seen below) as usual to run the test-suite (now including our custom testcase)
```
cd ~/cs3223-assign1 
./test-lru.sh
```