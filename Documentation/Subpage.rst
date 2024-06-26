Subpage support
===============

Subpage block size support, or just *subpage* for short, is a feature to allow
using a filesystem that has different size of data block size (*sectorsize*)
and the host CPU page size. For easier implementation the support was limited
to the exactly same size of the block and page. On x86_64 this is typically
4KiB, but there are other architectures commonly used that make use of larger
pages, like 64KiB on 64bit ARM or PowerPC. This means filesystems created
with 64KiB sector size cannot be mounted on a system with 4KiB page size.

While with subpage support systems with 64KiB page size can create
and mount filesystems with 4KiB sectorsize.  This still needs to use option "-s
4k" option for :command:`mkfs.btrfs`.

Requirements, limitations
-------------------------

The initial subpage support has been added in v5.15, although it's still
considered as experimental, most features are already working without problems.
On a 64KiB page system filesystem with 4KiB sectorsize can be mounted and used
as usual as long as the initial mount succeeds. There are cases a mount will be
rejected when verifying compatible features.

Please refer to status page of :ref:`status-subpage-block-size` for
compatibility.
