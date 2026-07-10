def will_init_target(target, init_sequence):
    # Renesas RA_DFP 6.4.0 has an RA8P1 SVD that pyOCD cannot parse cleanly.
    # SVD data is not required for flashing, so skip that init task.
    if init_sequence.has_task("load_svd"):
        init_sequence.remove_task("load_svd")
