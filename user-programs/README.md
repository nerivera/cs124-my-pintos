# User Programs
Fully functional implementation of user program support in Pintos (passes all tests). Includes argument passing (setting up the argument array on the user stack page) and safe user memory access in the system call infrastructure. System calls include:
- exec (similar to fork)
- wait (including support for exit codes)
- file syscalls (with support for file descriptors and denying writes to files in use as executables)

[Read the project spec here](http://users.cms.caltech.edu/~donnie/cs124/pintos_4.html#SEC53)
