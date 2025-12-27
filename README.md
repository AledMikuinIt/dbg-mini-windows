# Mini DBG 

# Work in progress (README & POC)

- retrive PEB & LDR from remote process
- list the modules (dll) from the process
- Attach debug mode on remote process
- Add a breakpoint (int3) & single step CPU flag
- Case handler for debug events
- Show adresses of the args

### Main exe

- Loop a MessageBoxA function to test the dbg


## Compiling
```
gcc -o target.exe target.c
```
```
gcc -o dbg.exe dbg.c
```

## Usage
Launch target.exe then :
```
.\dbg.exe target.exe
```