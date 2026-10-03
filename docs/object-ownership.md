# RinGPU object ownership and handle lifetime

`RinGpuHandle` is a 64-bit opaque value, not an owning C pointer. Copying it
with assignment creates another alias to the same object; it does not retain
the object or create an independently destroyable reference. RinGPU has no
public handle clone, retain, or move operation. A caller that transfers its
one logical owner should assign the value to the recipient and clear its own
variable. This is a caller convention only: any un-cleared copies remain
aliases, not additional owners.

Call `ringpu_destroy` once for the logical owner. On success the object slot is
released and its generation advances, so all copied aliases become stale and
subsequent use or destruction returns `RIN_GPU_ERROR_INVALID_HANDLE`. If the
object is still referenced by a dependent object or a recorded command list,
`ringpu_destroy` returns `RIN_GPU_ERROR_BUSY` without releasing the slot; the
handle remains valid and the caller may retry after releasing those
references. `BUSY` is not deferred destruction, and callers must not discard
the only live handle after receiving it.

Created pipelines retain their shader modules; bind groups retain their
pipeline and bound resources; recorded commands retain the objects they
reference. Destroy dependents before dependencies. Resetting or destroying a
command list releases the references recorded by that list. Core shutdown
releases remaining core-owned objects and invalidates their handles.

These CPU-side reference counts do not establish GPU completion. This
contract does not promise that a submitted object's storage may be destroyed
or a command list reset before the relevant completion fence; asynchronous
in-flight lifetime remains a separate backend requirement (see `RG-014`).

## Verified behavior

`ringpu-ownership` exercises a copied image handle, rejection of destruction
while a recorded command references the image, release on command-list reset,
successful destruction through one alias, and rejection of the stale alias.
This is host/software-backend coverage, not physical-GPU lifetime validation.
