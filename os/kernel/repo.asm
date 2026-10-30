; The app repository ("disco de Chamows"), built by tools/mkrepo.py.
section .rodata
align 16
global repo_blob
repo_blob:
    incbin "build/repo.bin"
section .note.GNU-stack noalloc noexec nowrite progbits
