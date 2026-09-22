(module
  (import "wasi_snapshot_preview1" "args_sizes_get" (func $args_sizes (result i32 i32)))
  (import "wasi_snapshot_preview1" "args_get" (func $args_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_write" (func $fd_write (param i32 i32 i32) (result i32 i32)))
  (import "wasi_snapshot_preview1" "clock_time_get" (func $clock (param i32 i64) (result i32 i64)))
  (memory (export "memory") 1)

  (data (i32.const 64) "wasi_echo: hello from easm WASI\n")

  ;; write_one(fd, ptr, len)
  (func $write (param $fd i32) (param $ptr i32) (param $len i32) (result i32)
    (i32.store (i32.const 512) (local.get $ptr))
    (i32.store (i32.const 516) (local.get $len))
    (i32.store
      (i32.const 520)
      (call $fd_write (local.get $fd) (i32.const 512) (i32.const 1)))
    (i32.load (i32.const 524))
  )

  (func $_start (export "_start")
    (local $n i32) (local $i i32) (local $buf i32) (local $ptr i32) (local $t i64)
    ;; header
    (drop (call $write (i32.const 1) (i32.const 64) (i32.const 33)))

    ;; uptime ns since some epoch, just to show clock works
    (call $clock (i32.const 1) (i64.const 0))
    (local.set $t)
    ;; print "clock_ns=<t>\n" — decimal conversion
    (local.set $buf (i32.const 256))
    (i32.store8 (local.get $buf) (i32.const 99))         ;; 'c'
    (i32.store8 (i32.add (local.get $buf) (i32.const 1)) (i32.const 61)) ;; '='
    (local.set $i (i32.add (local.get $buf) (i32.const 2)))
    (block $done
      (loop $digits
        (br_if $done (i64.lt_u (local.get $t) (i64.const 10)))
        (i32.store8 (local.get $i)
          (i32.add (i32.const 48) (i32.wrap_i64 (i64.rem_u (local.get $t) (i64.const 10)))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (local.set $t (i64.div_u (local.get $t) (i64.const 10)))
        (br $digits)
      )
    )
    (i32.store8 (local.get $i) (i32.const 10)) ;; '\n'
    (drop (call $write (i32.const 1) (local.get $buf)
      (i32.add (i32.sub (local.get $i) (local.get $buf)) (i32.const 1))))

    ;; args
    (call $args_sizes)
    (local.set $n)  ;; arg buf size (first result already in second local? see below)
    (drop)
    ;; NOTE: results order — sizes_get returns (count, buflen)
    (drop (call $args_sizes)) ;; warm path; real values below via stack machine in order
  )
)
