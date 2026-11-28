"""Run the same assertions against either COW permission model."""

import argparse
from itertools import product
import sys
import unittest

from permission_model import Leaf, Mapping, authorize_fixed, authorize_stale


class CowPermissionTest(unittest.TestCase):
    authorize = staticmethod(authorize_fixed)

    def test_stable_mapping_matrix(self):
        for vma_allows_write, leaf in product((False, True), Leaf):
            with self.subTest(vma_allows_write=vma_allows_write, leaf=leaf):
                mapping = Mapping(leaf, vma_allows_write)
                self.assertEqual(
                    self.authorize(mapping, mapping),
                    (vma_allows_write, leaf) == (True, Leaf.COW),
                )

    def test_read_only_replacement(self):
        self.assertFalse(self.authorize(Mapping(Leaf.COW), Mapping(Leaf.READ_ONLY)))

    def test_removed_mapping(self):
        self.assertFalse(self.authorize(Mapping(Leaf.COW), Mapping(Leaf.UNMAPPED)))

    def test_vma_write_permission_withdrawn(self):
        previous = Mapping(Leaf.COW)
        current = Mapping(Leaf.COW, vma_allows_write=False)
        self.assertFalse(self.authorize(previous, current))

    def test_already_writable_does_not_need_cow(self):
        self.assertFalse(self.authorize(Mapping(Leaf.COW), Mapping(Leaf.WRITABLE)))


if __name__ == "__main__":
    variants = {"stale": authorize_stale, "fixed": authorize_fixed}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", choices=variants)
    args = parser.parse_args()
    CowPermissionTest.authorize = staticmethod(variants[args.version])
    unittest.main(argv=[sys.argv[0]], verbosity=2)
