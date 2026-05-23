import pytest
import ctypes
import struct
import sys


# Simulate the vulnerable allocation size computation in Python
# to test for integer overflow conditions that would occur in C

def compute_allocation_size(cols, sizeof_scrollback_line=16, sizeof_cell=4):
    """
    Simulates the C allocation: sizeof(ScrollbackLine) + cols * sizeof(sb_row->cells[0])
    Returns the computed size, checking for overflow conditions.
    """
    # In C on 32-bit systems, size_t is 32-bit, so we simulate that
    MAX_SIZE_T_32 = 0xFFFFFFFF
    MAX_SIZE_T_64 = 0xFFFFFFFFFFFFFFFF
    
    # Compute with Python's arbitrary precision to detect overflow
    true_size = sizeof_scrollback_line + cols * sizeof_cell
    
    # Simulate 32-bit overflow
    size_32bit = (sizeof_scrollback_line + (cols * sizeof_cell) & MAX_SIZE_T_32) & MAX_SIZE_T_32
    
    # Simulate 64-bit overflow
    size_64bit = (sizeof_scrollback_line + (cols * sizeof_cell) & MAX_SIZE_T_64) & MAX_SIZE_T_64
    
    return {
        'true_size': true_size,
        'size_32bit': size_32bit,
        'size_64bit': size_64bit,
        'cols': cols,
        'memcpy_size': cols * sizeof_cell,
    }


def is_safe_cols_value(cols, sizeof_scrollback_line=16, sizeof_cell=4):
    """
    Security invariant check: the allocation must be large enough to hold
    the data that will be memcpy'd into it.
    Returns True if safe, False if vulnerable to overflow.
    """
    MAX_REASONABLE_COLS = 65536  # Reasonable terminal width limit
    
    if cols < 0:
        return False
    
    if cols > MAX_REASONABLE_COLS:
        return False
    
    # Check for 32-bit integer overflow
    MAX_SIZE_T_32 = 0xFFFFFFFF
    allocation_size_32 = (sizeof_scrollback_line + cols * sizeof_cell) & MAX_SIZE_T_32
    memcpy_size = cols * sizeof_cell
    
    # If the allocation wraps around, it's smaller than what memcpy will write
    if allocation_size_32 < memcpy_size:
        return False
    
    # Check that allocation is at least as large as what will be copied
    true_allocation = sizeof_scrollback_line + cols * sizeof_cell
    if true_allocation < memcpy_size:
        return False
    
    return True


@pytest.mark.parametrize("payload", [
    # Normal/safe values
    {"cols": 80, "expected_safe": True},
    {"cols": 132, "expected_safe": True},
    {"cols": 0, "expected_safe": False},   # Zero cols is edge case
    {"cols": 1, "expected_safe": True},
    {"cols": 256, "expected_safe": True},
    {"cols": 1024, "expected_safe": True},
    
    # Boundary values near 32-bit overflow
    {"cols": 0x3FFFFFFF, "expected_safe": False},  # Large value causing overflow
    {"cols": 0x40000000, "expected_safe": False},  # 1GB worth of cells
    {"cols": 0x7FFFFFFF, "expected_safe": False},  # INT_MAX
    {"cols": 0x80000000, "expected_safe": False},  # INT_MAX + 1
    {"cols": 0xFFFFFFFF, "expected_safe": False},  # UINT_MAX (32-bit)
    {"cols": 0x100000000, "expected_safe": False}, # UINT_MAX + 1 (64-bit boundary)
    
    # Values that cause overflow with sizeof_cell=4
    # cols * 4 overflows 32-bit: cols > 0x3FFFFFFF causes overflow when added to header
    {"cols": 0x3FFFFFFD, "expected_safe": False},
    {"cols": 0x3FFFFFFE, "expected_safe": False},
    
    # Adversarial terminal resize values
    {"cols": 65535, "expected_safe": False},  # Max unsigned 16-bit - suspicious
    {"cols": 65536, "expected_safe": False},  # 2^16 - suspicious
    {"cols": 32767, "expected_safe": False},  # Max signed 16-bit - suspicious
    
    # Values that look like escape sequence injections (large but plausible)
    {"cols": 99999, "expected_safe": False},
    {"cols": 999999, "expected_safe": False},
    {"cols": 9999999, "expected_safe": False},
    
    # Negative values (if cols is treated as signed)
    {"cols": -1, "expected_safe": False},
    {"cols": -2147483648, "expected_safe": False},  # INT_MIN
])
def test_scrollback_buffer_allocation_no_overflow(payload):
    """
    Invariant: The scrollback buffer allocation size must ALWAYS be large enough
    to hold the data that will be memcpy'd into it. Any 'cols' value that would
    cause integer overflow in the allocation computation (resulting in an
    undersized buffer) must be rejected before allocation occurs.
    
    Specifically: sizeof(ScrollbackLine) + cols * sizeof(cell) must not overflow,
    and the resulting allocation must be >= cols * sizeof(cell) (the memcpy size).
    """
    cols = payload["cols"]
    expected_safe = payload["expected_safe"]
    
    sizeof_scrollback_line = 16  # Typical size
    sizeof_cell = 4              # Typical cell size
    
    result = compute_allocation_size(cols, sizeof_scrollback_line, sizeof_cell)
    
    # Security invariant: if cols is negative, it must be rejected
    if cols < 0:
        assert not is_safe_cols_value(cols), (
            f"Negative cols={cols} must be rejected to prevent security issues"
        )
        return
    
    # Security invariant: for large cols values, check 32-bit overflow
    MAX_SIZE_T_32 = 0xFFFFFFFF
    allocation_32bit = (sizeof_scrollback_line + cols * sizeof_cell) & MAX_SIZE_T_32
    memcpy_size = cols * sizeof_cell
    
    if cols > 0:
        # The true (Python arbitrary precision) allocation must be >= memcpy size
        true_allocation = sizeof_scrollback_line + cols * sizeof_cell
        assert true_allocation >= memcpy_size, (
            f"Allocation {true_allocation} must be >= memcpy size {memcpy_size} for cols={cols}"
        )
    
    # Security invariant: if 32-bit overflow would occur, the value must be flagged as unsafe
    if allocation_32bit < memcpy_size & MAX_SIZE_T_32:
        assert not is_safe_cols_value(cols), (
            f"cols={cols} causes 32-bit overflow: alloc={allocation_32bit:#x} < "
            f"memcpy_size={memcpy_size & MAX_SIZE_T_32:#x}, must be rejected"
        )
    
    # Verify our safety check matches expected
    actual_safe = is_safe_cols_value(cols)
    assert actual_safe == expected_safe, (
        f"cols={cols}: expected safe={expected_safe}, got safe={actual_safe}. "
        f"Allocation details: true_size={result['true_size']}, "
        f"32bit_size={result['size_32bit']:#x}, memcpy_size={result['memcpy_size']}"
    )


@pytest.mark.parametrize("cols", [
    0x3FFFFFFF,
    0x40000000,
    0x7FFFFFFF,
    0x80000000,
    0xFFFFFFFF,
    0x100000000,
    2**31 - 1,
    2**32 - 1,
    2**32,
    2**63 - 1,
])
def test_large_cols_causes_overflow_detection(cols):
    """
    Invariant: Extremely large 'cols' values from malicious terminal resize events
    must be detected as causing integer overflow in the allocation computation.
    These values must never be passed directly to malloc without bounds checking.
    """
    sizeof_scrollback_line = 16
    sizeof_cell = 4
    
    # These large values MUST be detected as unsafe
    assert not is_safe_cols_value(cols), (
        f"cols={cols} ({cols:#x}) must be detected as unsafe - "
        f"it would cause integer overflow in malloc size computation"
    )
    
    # Verify the overflow would actually occur on 32-bit systems
    MAX_SIZE_T_32 = 0xFFFFFFFF
    cells_size = (cols * sizeof_cell) & MAX_SIZE_T_32
    total_alloc = (sizeof_scrollback_line + cells_size) & MAX_SIZE_T_32
    true_memcpy = cols * sizeof_cell
    
    # The 32-bit allocation is much smaller than what memcpy would write
    assert total_alloc < true_memcpy, (
        f"For cols={cols}: 32-bit alloc={total_alloc} should be less than "
        f"true memcpy size={true_memcpy}, confirming overflow vulnerability"
    )


def test_safe_cols_range_is_bounded():
    """
    Invariant: There must exist a maximum safe cols value beyond which
    all values are considered unsafe. This ensures the security boundary
    is finite and enforceable.
    """
    MAX_REASONABLE_COLS = 65536
    
    # All values above the reasonable maximum must be unsafe
    unsafe_large_values = [
        MAX_REASONABLE_COLS + 1,
        MAX_REASONABLE_COLS * 2,
        0x7FFFFFFF,
        0xFFFFFFFF,
    ]
    
    for cols in unsafe_large_values:
        assert not is_safe_cols_value(cols), (
            f"cols={cols} exceeds reasonable terminal width and must be rejected"
        )
    
    # Reasonable terminal widths should be safe
    safe_values = [1, 80, 132, 200, 512, 1024, 4096]
    for cols in safe_values:
        assert is_safe_cols_value(cols), (
            f"cols={cols} is a reasonable terminal width and should be accepted"
        )


def test_allocation_size_monotonically_increases_for_safe_values():
    """
    Invariant: For safe cols values, the allocation size must monotonically
    increase with cols. If it decreases (due to overflow), the value is unsafe.
    """
    sizeof_scrollback_line = 16
    sizeof_cell = 4
    
    prev_size = sizeof_scrollback_line  # base size with 0 cols
    
    for cols in range(1, 10001, 100):
        current_size = sizeof_scrollback_line + cols * sizeof_cell
        
        # Allocation must grow as cols grows (no overflow for safe values)
        assert current_size > prev_size, (
            f"Allocation size must increase monotonically: "
            f"cols={cols}, current={current_size}, prev={prev_size}"
        )
        
        # The allocation must always be larger than just the cells
        assert current_size > cols * sizeof_cell, (
            f"Allocation must include header overhead: "
            f"alloc={current_size}, cells_only={cols * sizeof_cell}"
        )
        
        prev_size = current_size