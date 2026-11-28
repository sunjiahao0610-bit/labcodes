"""Compare two COW authorization decisions using symbolic states."""

from dataclasses import dataclass
from enum import Enum, auto


class Leaf(Enum):
    UNMAPPED = auto()
    READ_ONLY = auto()
    COW = auto()
    WRITABLE = auto()


@dataclass(frozen=True)
class Mapping:
    leaf: Leaf
    vma_allows_write: bool = True


def may_resolve_cow(mapping):
    return mapping.vma_allows_write and mapping.leaf is Leaf.COW


def authorize_stale(previous, current):
    """Model an authorization decision that reuses an earlier result."""
    return may_resolve_cow(previous)


def authorize_fixed(previous, current):
    """Authorize COW resolution using the current mapping and policy."""
    return may_resolve_cow(current)
