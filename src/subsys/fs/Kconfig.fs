config EERIE_LEAP_FS
    bool "EerieLeap FS"
    # SdmmcService, built with DISK_ACCESS, polls the card on its own thread.
    select EERIE_LEAP_THREADING if DISK_ACCESS
    help
      Add EerieLeap FS.

menu "EerieLeap FS config"
    depends on EERIE_LEAP_FS

    config EERIE_LEAP_FS_SD_THREAD_STACK_SIZE
        int "SD thread stack size"
        default 3072

    config EERIE_LEAP_SD_CHECK_INTERVAL_MS
        int "SD check interval ms"
        default 1000

    config EERIE_LEAP_FS_STREAM_WRITE_BUFFER_SIZE
        int "Stream write buffer size"
        range 0 65536
        default 4096
        help
          Bytes FsServiceStreamBuf collects before issuing one fs_write() in
          write and append modes; 0 hands every write straight to the file
          system. A larger buffer means fewer, larger writes, not a larger
          loss window: the buffer is flushed on every sync, seek and close, so
          what a power loss can take is bounded by how often the writer syncs
          (EERIE_LEAP_LOG_SYNC_INTERVAL_MS for the log writer) plus at most
          one buffer of records.
endmenu
