# RinGPU object ownership and handle lifetime

`RinGpuHandle` is a 64-bit opaque value, not an owning C pointer. Copying it
with assignment creates another alias to the same object; it does not retain
the object or create an independently destroyable reference. RinGPU has no
public handle clone, retain, or move operation. A caller that transfers its
one logical owner should assign the value to the recipient and clear its own
variable. This is a caller convention only: any un-cleared copies remain
aliases, not additional owners.

Call `ringpu_destroy` once for the logical owner. On success the handle becomes
invalid immediately, so all copied aliases return
`RIN_GPU_ERROR_INVALID_HANDLE`. If the object is referenced only by dependent
objects or command lists that have not been successfully submitted,
`ringpu_destroy` returns `RIN_GPU_ERROR_BUSY` and the caller may retry after
releasing those references. If an accepted submission references the object,
destroy succeeds logically but retains its native allocation in a deferred
retirement state. Retained command-list and dependency references must also be
released before the native object is destroyed.

Created pipelines retain their shader modules; bind groups retain their
pipeline and bound resources; recorded commands retain the objects they
reference. Destroy dependents before dependencies. Resetting or destroying a
command list releases the references recorded by that list. The core stamps
the submitted command-list references and their dependency graph with an
internal submission serial. A successful `ringpu_wait_fence` means the
backend's completion callback has completed all work submitted before the
wait; only then can the core retire objects whose final-use serial is covered.
A timeout or backend error does not advance completion or release deferred
objects. Backends without a completion callback must complete submissions
synchronously before `submit_commands` returns. Core shutdown makes a final
best-effort completion wait before destroying backend objects.

CPU-side reference counts and GPU completion serials are separate protections:
recorded/dependent references prevent premature handle destruction, while the
serial prevents native storage from being freed before GPU completion.

## Verified behavior

`ringpu-ownership` exercises a copied image handle, `BUSY` for an unsubmitted
recorded reference, deferred handle invalidation after submit, timeout without
retirement, completion-triggered retirement after reset, and rejection of the
stale alias. This is host/software-backend coverage, not physical-GPU lifetime
validation.
