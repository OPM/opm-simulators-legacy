/*
  Copyright 2013 SINTEF ICT, Applied Mathematics.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>

#if HAVE_DYNAMIC_BOOST_TEST
#define BOOST_TEST_DYN_LINK
#endif

#define BOOST_TEST_MODULE AutoDiffBlockTest

#include <opm/autodiff/AutoDiffBlock.hpp>

#include <boost/test/unit_test.hpp>

#include <Eigen/Eigen>
#include <Eigen/Sparse>

using namespace Opm;

namespace {
    template <typename Scalar>
    bool
    operator ==(const Eigen::SparseMatrix<Scalar>& A,
                const Eigen::SparseMatrix<Scalar>& B)
    {
        // Two SparseMatrices are equal if
        //   0) They have the same ordering (enforced by equal types)
        //   1) They have the same outer and inner dimensions
        //   2) They have the same number of non-zero elements
        //   3) They have the same sparsity structure
        //   4) The non-zero elements are equal

        // 1) Outer and inner dimensions
        bool eq =       (A.outerSize() == B.outerSize());
        eq      = eq && (A.innerSize() == B.innerSize());

        // 2) Equal number of non-zero elements
        eq      = eq && (A.nonZeros() == B.nonZeros());

        for (typename Eigen::SparseMatrix<Scalar>::Index
                 k0 = 0, kend = A.outerSize(); eq && (k0 < kend); ++k0) {
            for (typename Eigen::SparseMatrix<Scalar>::InnerIterator
                     iA(A, k0), iB(B, k0); eq && (iA && iB); ++iA, ++iB) {
                // 3) Sparsity structure
                eq = (iA.row() == iB.row()) && (iA.col() == iB.col());

                // 4) Equal non-zero elements
                eq = eq && (iA.value() == iB.value());
            }
        }

        return eq;

        // Note: Investigate implementing this operator as
        // return A.cwiseNotEqual(B).count() == 0;
    }
}

BOOST_AUTO_TEST_CASE(ConstantInitialisation)
{
    typedef AutoDiffBlock<double> ADB;

    ADB::V v(3);
    v << 0.2, 1.2, 13.4;

    ADB a = ADB::constant(v);
    BOOST_REQUIRE(a.value().matrix() == v.matrix());

    const std::vector<ADB::M>& J = a.derivative();
    for (std::vector<ADB::M>::const_iterator
             b = J.begin(), e = J.end(); b != e; ++b) {
        BOOST_REQUIRE(b->nonZeros() == 0);
    }
}

BOOST_AUTO_TEST_CASE(VariableInitialisation)
{
    typedef AutoDiffBlock<double> ADB;

    std::vector<int> blocksizes = { 3, 1, 2 };

    ADB::V v(3);
    v << 1.0, 2.2, 3.4;

    enum { FirstVar = 0, SecondVar = 1, ThirdVar = 2 };

    ADB x = ADB::variable(FirstVar, v, blocksizes);

    BOOST_REQUIRE(x.value().matrix() == v.matrix());

    const std::vector<ADB::M>& J = x.derivative();
    BOOST_REQUIRE(J[0].nonZeros() == v.size());

    const Eigen::Diagonal<const ADB::M, 0>& d = J[0].diagonal();
    BOOST_REQUIRE((d.array() == 1.0).all());

    for (std::vector<ADB::M>::const_iterator
             b = J.begin() + 1, e = J.end(); b != e; ++b) {
        BOOST_REQUIRE(b->nonZeros() == 0);
    }
}

BOOST_AUTO_TEST_CASE(FunctionInitialisation)
{
    typedef AutoDiffBlock<double> ADB;

    std::vector<int>            blocksizes = { 3, 1, 2 };
    std::vector<int>::size_type num_blocks = blocksizes.size();

    enum { FirstVar = 0, SecondVar = 1, ThirdVar = 2 };

    ADB::V v(3);
    v << 1.0, 2.2, 3.4;

    std::vector<ADB::M> jacs(num_blocks);
    for (std::vector<int>::size_type j = 0; j < num_blocks; ++j) {
        jacs[j] = ADB::M(blocksizes[FirstVar], blocksizes[j]);
        jacs[j].insert(0,0) = -1.0;
    }

    ADB::V v_copy(v);
    std::vector<ADB::M> jacs_copy(jacs);
    ADB f = ADB::function(std::move(v_copy), std::move(jacs_copy));

    BOOST_REQUIRE(f.value().matrix() == v.matrix());

    const std::vector<ADB::M>& J = f.derivative();
    for (std::vector<ADB::M>::const_iterator
             bf = J.begin(), ef = J.end(), bj = jacs.begin();
         bf != ef; ++bf, ++bj) {

        BOOST_CHECK(*bf == *bj);
    }
}

BOOST_AUTO_TEST_CASE(Addition)
{
    typedef AutoDiffBlock<double> ADB;
    std::vector<int> blocksizes = { 3, 1, 2 };

    ADB::V va(3);
    va << 0.2, 1.2, 13.4;

    ADB::V vx(3);
    vx << 1.0, 2.2, 3.4;

    enum { FirstVar = 0, SecondVar = 1, ThirdVar = 2 };

    ADB a = ADB::constant(va, blocksizes);
    ADB x = ADB::variable(FirstVar, vx, blocksizes);

    ADB xpx = x + x;

    BOOST_CHECK_EQUAL(xpx.value().cwiseNotEqual(2 * x.value()).count(), 0);

    const std::vector<ADB::M>& J1x = x  .derivative();
    const std::vector<ADB::M>& J2x = xpx.derivative();
    BOOST_CHECK_EQUAL(J1x.size(), J2x.size());
    for (std::vector<ADB::M>::const_iterator
             j1b = J1x.begin(), j1e = J1x.end(), j2b = J2x.begin();
         j1b != j1e; ++j1b, ++j2b) {
        BOOST_CHECK(*j2b == ADB::M((*j1b) * 2));
    }

    ADB::V  r = 2*x.value() + a.value();
    ADB xpxpa = x + x + a;
    BOOST_CHECK_EQUAL(xpxpa.value().cwiseNotEqual(r).count(), 0);

    const std::vector<ADB::M>& J3 = xpxpa.derivative();
    for (std::vector<ADB::M>::const_iterator
             j1b = J1x.begin(), j1e = J1x.end(), j3b = J3.begin();
         j1b != j1e; ++j1b, ++j3b) {
        BOOST_CHECK(*j3b == ADB::M((*j1b) * 2));
    }
}

BOOST_AUTO_TEST_CASE(AssignAddSubtractOperators)
{
    typedef AutoDiffBlock<double> ADB;

    // Basic testing of += and -=.
    ADB::V vx(3);
    vx << 0.2, 1.2, 13.4;

    ADB::V vy(3);
    vy << 1.0, 2.2, 3.4;

    std::vector<ADB::V> vals{ vx, vy };
    std::vector<ADB> vars = ADB::variables(vals);

    const ADB x = vars[0];
    const ADB y = vars[1];

    ADB z = x;
    z += y;
    ADB sum = x + y;
    const double tolerance = 1e-14;
    BOOST_CHECK(z.value().isApprox(sum.value(), tolerance));
    BOOST_CHECK(z.derivative()[0].isApprox(sum.derivative()[0], tolerance));
    BOOST_CHECK(z.derivative()[1].isApprox(sum.derivative()[1], tolerance));
    z -= y;
    BOOST_CHECK(z.value().isApprox(x.value(), tolerance));
    BOOST_CHECK(z.derivative()[0].isApprox(x.derivative()[0], tolerance));
    BOOST_CHECK(z.derivative()[1].isApprox(x.derivative()[1], tolerance));

    // Testing the case when the left hand side has empty() jacobian.
    ADB yconst = ADB::constant(vy);
    z = yconst;
    z -= x;
    ADB diff = yconst - x;
    BOOST_CHECK(z.value().isApprox(diff.value(), tolerance));
    BOOST_CHECK(z.derivative()[0].isApprox(diff.derivative()[0], tolerance));
    BOOST_CHECK(z.derivative()[1].isApprox(diff.derivative()[1], tolerance));
    z += x;
    BOOST_CHECK(z.value().isApprox(yconst.value(), tolerance));
    BOOST_CHECK(z.derivative()[0].isApprox(Eigen::Matrix<double, 3, 3>::Zero()));
    BOOST_CHECK(z.derivative()[1].isApprox(Eigen::Matrix<double, 3, 3>::Zero()));
}
