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

#ifndef OPM_AUTODIFFHELPERS_HEADER_INCLUDED
#define OPM_AUTODIFFHELPERS_HEADER_INCLUDED

#include <opm/autodiff/AutoDiffBlock.hpp>
#include <opm/autodiff/GridHelpers.hpp>
#include <opm/core/grid.h>
#include <opm/core/utility/ErrorMacros.hpp>

#include <iostream>
#include <vector>

namespace Opm
{

// -------------------- class HelperOps --------------------

/// Contains vectors and sparse matrices that represent subsets or
/// operations on (AD or regular) vectors of data.
struct HelperOps
{
    typedef AutoDiffBlock<double>::M M;
    typedef AutoDiffBlock<double>::V V;

    /// A list of internal faces.
    typedef Eigen::Array<int, Eigen::Dynamic, 1> IFaces;
    IFaces internal_faces;

    /// Extract for each internal face the difference of its adjacent cells' values (first - second).
    M ngrad;
    /// Extract for each face the difference of its adjacent cells' values (second - first).
    M grad;
    /// Extract for each face the average of its adjacent cells' values.
    M caver;
    /// Extract for each cell the sum of its adjacent interior faces' (signed) values.
    M div;
    /// Extract for each face the difference of its adjacent cells' values (first - second).
    /// For boundary faces, one of the entries per row (corresponding to the outside) is zero.
    M fullngrad;
    /// Extract for each cell the sum of all its adjacent faces' (signed) values.
    M fulldiv;

    /// Constructs all helper vectors and matrices.
    template<class Grid>
    HelperOps(const Grid& grid)
    {
        using namespace AutoDiffGrid;
        const int nc = numCells(grid);
        const int nf = numFaces(grid);
        // Define some neighbourhood-derived helper arrays.
        typedef Eigen::Array<int, Eigen::Dynamic, 2, Eigen::RowMajor> TwoColInt;
        TwoColInt nbi;
        extractInternalFaces(grid, internal_faces, nbi);
        int num_internal=internal_faces.size();
        
        // std::cout << "nbi = \n" << nbi << std::endl;
        // Create matrices.
        ngrad.resize(num_internal, nc);
        caver.resize(num_internal, nc);
        typedef Eigen::Triplet<double> Tri;
        std::vector<Tri> ngrad_tri;
        std::vector<Tri> caver_tri;
        ngrad_tri.reserve(2*num_internal);
        caver_tri.reserve(2*num_internal);
        for (int i = 0; i < num_internal; ++i) {
            ngrad_tri.emplace_back(i, nbi(i,0), 1.0);
            ngrad_tri.emplace_back(i, nbi(i,1), -1.0);
            caver_tri.emplace_back(i, nbi(i,0), 0.5);
            caver_tri.emplace_back(i, nbi(i,1), 0.5);
        }
        ngrad.setFromTriplets(ngrad_tri.begin(), ngrad_tri.end());
        caver.setFromTriplets(caver_tri.begin(), caver_tri.end());
        grad = -ngrad;
        div = ngrad.transpose();
        std::vector<Tri> fullngrad_tri;
        fullngrad_tri.reserve(2*nf);
        typename ADFaceCellTraits<Grid>::Type nb=faceCells(grid);
        for (int i = 0; i < nf; ++i) {
            if (nb(i,0) >= 0) {
                fullngrad_tri.emplace_back(i, nb(i,0), 1.0);
            }
            if (nb(i,1) >= 0) {
                fullngrad_tri.emplace_back(i, nb(i,1), -1.0);
            }
        }
        fullngrad.resize(nf, nc);
        fullngrad.setFromTriplets(fullngrad_tri.begin(), fullngrad_tri.end());
        fulldiv = fullngrad.transpose();
    }
};

// -------------------- upwinding helper class --------------------


    /// Upwind selection in absence of counter-current flow (i.e.,
    /// without effects of gravity and/or capillary pressure).
    template <typename Scalar>
    class UpwindSelector {
    public:
        typedef AutoDiffBlock<Scalar> ADB;

        template<class Grid>
        UpwindSelector(const Grid& g,
                       const HelperOps&        h,
                       const typename ADB::V&  ifaceflux)
        {
            using namespace AutoDiffGrid;
            typedef HelperOps::IFaces::Index IFIndex;
            const IFIndex nif = h.internal_faces.size();
            typename ADFaceCellTraits<Grid>::Type
                face_cells = faceCells(g);
            assert(nif == ifaceflux.size());

            // Define selector structure.
            typedef typename Eigen::Triplet<Scalar> Triplet;
            std::vector<Triplet> s;  s.reserve(nif);
            for (IFIndex iface = 0; iface < nif; ++iface) {
                const int f  = h.internal_faces[iface];
                const int c1 = face_cells(f,0);
                const int c2 = face_cells(f,1);

                assert ((c1 >= 0) && (c2 >= 0));

                // Select upwind cell.
                const int c = (ifaceflux[iface] >= 0) ? c1 : c2;

                s.push_back(Triplet(iface, c, Scalar(1)));
            }

            // Assemble explicit selector operator.
            select_.resize(nif, numCells(g));
            select_.setFromTriplets(s.begin(), s.end());
        }

        /// Apply selector to multiple per-cell quantities.
        std::vector<ADB>
        select(const std::vector<ADB>& xc) const
        {
            // Absence of counter-current flow means that the same
            // selector applies to all quantities, 'x', defined per
            // cell.
            std::vector<ADB> xf;  xf.reserve(xc.size());
            for (typename std::vector<ADB>::const_iterator
                     b = xc.begin(), e = xc.end(); b != e; ++b)
            {
                xf.push_back(select_ * (*b));
            }

            return xf;
        }

        /// Apply selector to single per-cell ADB quantity.
        ADB select(const ADB& xc) const
        {
            return select_*xc;
        }

        /// Apply selector to single per-cell constant quantity.
        typename ADB::V select(const typename ADB::V& xc) const
        {
            return (select_*xc.matrix()).array();
        }

    private:
        typename ADB::M select_;
    };



namespace {

    template <typename Scalar, class IntVec>
    Eigen::SparseMatrix<Scalar>
    constructSubsetSparseMatrix(const int full_size, const IntVec& indices)
    {
        typedef Eigen::Triplet<Scalar> Tri;
        const int subset_size = indices.size();
        std::vector<Tri> triplets(subset_size);
        for (int i = 0; i < subset_size; ++i) {
            triplets[i] = Tri(i, indices[i], 1);
        }
        Eigen::SparseMatrix<Scalar> sub(subset_size, full_size);
        sub.setFromTriplets(triplets.begin(), triplets.end());
        return sub;
    }

    template <typename Scalar, class IntVec>
    Eigen::SparseMatrix<Scalar>
    constructSupersetSparseMatrix(const int full_size, const IntVec& indices)
    {
        return constructSubsetSparseMatrix<Scalar>(full_size, indices).transpose();
    }

} // anon namespace


/// Returns x(indices).
template <typename Scalar, class IntVec>
AutoDiffBlock<Scalar>
subset(const AutoDiffBlock<Scalar>& x,
       const IntVec& indices)
{
    return constructSubsetSparseMatrix<Scalar>(x.value().size(), indices) * x;
}



/// Returns x(indices).
template <typename Scalar, class IntVec>
Eigen::Array<Scalar, Eigen::Dynamic, 1>
subset(const Eigen::Array<Scalar, Eigen::Dynamic, 1>& x,
       const IntVec& indices)
{
    return (constructSubsetSparseMatrix<Scalar>(x.size(), indices) * x.matrix()).array();
}



/// Returns v where v(indices) == x, v(!indices) == 0 and v.size() == n.
template <typename Scalar, class IntVec>
AutoDiffBlock<Scalar>
superset(const AutoDiffBlock<Scalar>& x,
         const IntVec& indices,
         const int n)
{
    return constructSupersetSparseMatrix<Scalar>(n, indices) * x;
}



/// Returns v where v(indices) == x, v(!indices) == 0 and v.size() == n.
template <typename Scalar, class IntVec>
Eigen::Array<Scalar, Eigen::Dynamic, 1>
superset(const Eigen::Array<Scalar, Eigen::Dynamic, 1>& x,
         const IntVec& indices,
         const int n)
{
    return constructSupersetSparseMatrix<Scalar>(n, indices) * x.matrix();
}



/// Construct square sparse matrix with the
/// elements of d on the diagonal.
/// Need to mark this as inline since it is defined in a header and not a template.
inline
AutoDiffBlock<double>::M
spdiag(const AutoDiffBlock<double>::V& d)
{
    typedef AutoDiffBlock<double>::M M;

    const int n = d.size();
    M mat(n, n);
    mat.reserve(Eigen::ArrayXi::Ones(n, 1));
    for (M::Index i = 0; i < n; ++i) {
        mat.insert(i, i) = d[i];
    }

    return mat;
}




    /// Selection. Choose first of two elements if selection basis element is nonnegative.
    template <typename Scalar>
    class Selector {
    public:
        typedef AutoDiffBlock<Scalar> ADB;

        enum CriterionForLeftElement { GreaterEqualZero, GreaterZero, Zero, NotEqualZero, LessZero, LessEqualZero };

        Selector(const typename ADB::V& selection_basis,
                 CriterionForLeftElement crit = GreaterEqualZero)
        {
            // Define selector structure.
            const int n = selection_basis.size();
            // Over-reserving so we do not have to count.
            left_elems_.reserve(n);
            right_elems_.reserve(n);
            for (int i = 0; i < n; ++i) {
                bool chooseleft = false;
                switch (crit) {
                case GreaterEqualZero:
                    chooseleft = selection_basis[i] >= 0.0;
                    break;
                case GreaterZero:
                    chooseleft = selection_basis[i] > 0.0;
                    break;
                case Zero:
                    chooseleft = selection_basis[i] == 0.0;
                    break;
                case NotEqualZero:
                    chooseleft = selection_basis[i] != 0.0;
                    break;
                case LessZero:
                    chooseleft = selection_basis[i] < 0.0;
                    break;
                case LessEqualZero:
                    chooseleft = selection_basis[i] <= 0.0;
                    break;
                default:
                    OPM_THROW(std::logic_error, "No such criterion: " << crit);
                }
                if (chooseleft) {
                    left_elems_.push_back(i);
                } else {
                    right_elems_.push_back(i);
                }
            }
        }

        /// Apply selector to ADB quantities.
        ADB select(const ADB& x1, const ADB& x2) const
        {
            if (right_elems_.empty()) {
                return x1;
            } else if (left_elems_.empty()) {
                return x2;
            } else {
                return superset(subset(x1, left_elems_), left_elems_, x1.size())
                    + superset(subset(x2, right_elems_), right_elems_, x2.size());
            }
        }

        /// Apply selector to ADB quantities.
        typename ADB::V select(const typename ADB::V& x1, const typename ADB::V& x2) const
        {
            if (right_elems_.empty()) {
                return x1;
            } else if (left_elems_.empty()) {
                return x2;
            } else {
                return superset(subset(x1, left_elems_), left_elems_, x1.size())
                    + superset(subset(x2, right_elems_), right_elems_, x2.size());
            }
        }

    private:
        std::vector<int> left_elems_;
        std::vector<int> right_elems_;
    };




/// Returns the input expression, but with all Jacobians collapsed to one.
inline
AutoDiffBlock<double>
collapseJacs(const AutoDiffBlock<double>& x)
{
    typedef AutoDiffBlock<double> ADB;
    const int nb = x.numBlocks();
    typedef Eigen::Triplet<double> Tri;
    int nnz = 0;
    for (int block = 0; block < nb; ++block) {
        nnz += x.derivative()[block].nonZeros();
    }
    std::vector<Tri> t;
    t.reserve(nnz);
    int block_col_start = 0;
    for (int block = 0; block < nb; ++block) {
        const ADB::M& jac = x.derivative()[block];
        for (ADB::M::Index k = 0; k < jac.outerSize(); ++k) {
            for (ADB::M::InnerIterator i(jac, k); i ; ++i) {
                t.push_back(Tri(i.row(),
                                i.col() + block_col_start,
                                i.value()));
            }
        }
        block_col_start += jac.cols();
    }
    // Build final jacobian.
    std::vector<ADB::M> jacs(1);
    jacs[0].resize(x.size(), block_col_start);
    jacs[0].setFromTriplets(t.begin(), t.end());
    return ADB::function(x.value(), jacs);
}




/// Returns the vertical concatenation [ x; y ] of the inputs.
inline
AutoDiffBlock<double>
vertcat(const AutoDiffBlock<double>& x,
        const AutoDiffBlock<double>& y)
{
    const int nx = x.size();
    const int ny = y.size();
    const int n = nx + ny;
    std::vector<int> xind(nx);
    for (int i = 0; i < nx; ++i) {
        xind[i] = i;
    }
    std::vector<int> yind(ny);
    for (int i = 0; i < ny; ++i) {
        yind[i] = nx + i;
    }
    return superset(x, xind, n) + superset(y, yind, n);
}





class Span
{
public:
    explicit Span(const int num)
    : num_(num),
      stride_(1),
      start_(0)
    {
    }
    Span(const int num, const int stride, const int start)
        : num_(num),
          stride_(stride),
          start_(start)
    {
    }
    int operator[](const int i) const
    {
        assert(i >= 0 && i < num_);
        return start_ + i*stride_;
    }
    int size() const
    {
        return num_;
    }


    class SpanIterator
    {
    public:
        SpanIterator(const Span* span, const int index)
            : span_(span),
              index_(index)
        {
        }
        SpanIterator operator++()
        {
            ++index_;
            return *this;
        }
        SpanIterator operator++(int)
        {
            SpanIterator before_increment(*this);
            ++index_;
            return before_increment;
        }
        bool operator<(const SpanIterator& rhs) const
        {
            assert(span_ == rhs.span_);
            return index_ < rhs.index_;
        }
        bool operator==(const SpanIterator& rhs) const
        {
            assert(span_ == rhs.span_);
            return index_ == rhs.index_;
        }
        bool operator!=(const SpanIterator& rhs) const
        {
            assert(span_ == rhs.span_);
            return index_ != rhs.index_;
        }
        int operator*()
        {
            return (*span_)[index_];
        }
    private:
        const Span* span_;
        int index_;
    };

    typedef SpanIterator iterator;
    typedef SpanIterator const_iterator;

    SpanIterator begin() const
    {
        return SpanIterator(this, 0);
    }

    SpanIterator end() const
    {
        return SpanIterator(this, num_);
    }

    bool operator==(const Span& rhs)
    {
        return num_ == rhs.num_ && start_ == rhs.start_ && stride_ == rhs.stride_;
    }

private:
    const int num_;
    const int stride_;
    const int start_;
};



/// Return a vector of (-1.0, 0.0 or 1.0), depending on sign per element.
inline Eigen::ArrayXd sign (const Eigen::ArrayXd& x)
{
    const int n = x.size();
    Eigen::ArrayXd retval(n);
    for (int i = 0; i < n; ++i) {
        retval[i] = x[i] < 0.0 ? -1.0 : (x[i] > 0.0 ? 1.0 : 0.0);
    }
    return retval;
}

} // namespace Opm

#endif // OPM_AUTODIFFHELPERS_HEADER_INCLUDED
