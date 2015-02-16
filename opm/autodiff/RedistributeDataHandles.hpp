/*
  Copyright 2015 Dr. Blatt - HPC-Simulation-Software & Services.
  Coypright 2015 NTNU

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


#include <opm/autodiff/BlackoilPropsAdFromDeck.hpp>
#include <opm/core/simulator/BlackoilState.hpp>

namespace Opm
{
/// \brief a data handle to distribute the BlackoilState
class BlackoilStateDataHandle
{
public:
    /// \brief The data that we send.
    typedef double DataType;
    /// \brief Constructor.
    /// \param sendGrid   The grid that the data is attached to when sending.
    /// \param recvGrid   The grid that the data is attached to when receiving.
    /// \param sendState  The state where we will retieve the values to be sent.
    /// \parame recvState The state where we will store the received values.
    BlackoilStateDataHandle(const Dune::CpGrid& sendGrid,
                            const Dune::CpGrid& recvGrid,
                            const BlackoilState& sendState,
                            BlackoilState& recvState)
        : sendGrid_(sendGrid), recvGrid_(recvGrid), sendState_(sendState), recvState_(recvState)
    {}

    bool fixedsize(int /*dim*/, int /*codim*/)
    {
        return false;
    }

    template<class T>
    std::size_t size(const T& e)
    {
        if ( T::codimension == 0)
        {
            return 2 * sendState_.numPhases() +4+2*sendGrid_.numCellFaces(e.index());
        }
        else
        {
            OPM_THROW(std::logic_error, "Data handle can only be used for elements");
        }
    }

    template<class B, class T>
    void gather(B& buffer, const T& e)
    {
        assert( T::codimension == 0);

        for ( int i=0; i<sendState_.numPhases(); ++i )
        {
            buffer.write(sendState_.surfacevol()[e.index()*sendState_.numPhases()+i]);
        }
        buffer.write(sendState_.gasoilratio()[e.index()]);
        buffer.write(sendState_.rv()[e.index()]);
        buffer.write(sendState_.pressure()[e.index()]);
        buffer.write(sendState_.temperature()[e.index()]);
        buffer.write(sendState_.saturation()[e.index()]);

        for ( int i=0; i<sendGrid_.numCellFaces(e.index()); ++i )
        {
            buffer.write(sendState_.facepressure()[sendGrid_.cellFace(e.index(), i)]);
        }
        for ( int i=0; i<sendGrid_.numCellFaces(e.index()); ++i )
        {
            buffer.write(recvState_.faceflux()[sendGrid_.cellFace(e.index(), i)]);
        }
    }
    template<class B, class T>
    void scatter(B& buffer, const T& e, std::size_t size)
    {
        assert( T::codimension == 0);
        assert( size == 2 * recvState_.numPhases() +4+2*recvGrid_.numCellFaces(e.index()));
        (void) size;

        for ( int i=0; i<recvState_.numPhases(); ++i )
        {
            double val;
            buffer.read(val);
            recvState_.surfacevol()[e.index()]=val;
        }
        double val;
        buffer.read(val);
        recvState_.gasoilratio()[e.index()]=val;
        buffer.read(val);
        recvState_.rv()[e.index()]=val;
        buffer.read(val);
        recvState_.pressure()[e.index()]=val;
        buffer.read(val);
        recvState_.temperature()[e.index()]=val;
        buffer.read(val);
        recvState_.saturation()[e.index()]=val;

        for ( int i=0; i<recvGrid_.numCellFaces(e.index()); ++i )
        {
            double val;
            buffer.read(val);
            recvState_.facepressure()[recvGrid_.cellFace(e.index(), i)]=val;
        }
        for ( int i=0; i<recvGrid_.numCellFaces(e.index()); ++i )
        {
            double val;
            buffer.read(val);
            recvState_.faceflux()[recvGrid_.cellFace(e.index(), i)]=val;
        }
    }
    bool contains(int dim, int codim)
    {
        return dim==3 && codim==0;
    }
private:
    /// \brief The grid that the data is attached to when sending
    const Dune::CpGrid& sendGrid_;
    /// \brief The grid that the data is attached to when receiving
    const Dune::CpGrid& recvGrid_;
    /// \brief The state where we will retieve the values to be sent.
    const BlackoilState& sendState_;
    // \brief The state where we will store the received values.
    BlackoilState& recvState_;
};

class BlackoilPropsDataHandle
{
public:
    /// \brief The data that we send.
    typedef double DataType;
    /// \brief Constructor.
    /// \param sendGrid   The grid that the data is attached to when sending.
    /// \param recvGrid   The grid that the data is attached to when receiving.
    /// \param sendProps  The properties where we will retieve the values to be sent.
    /// \parame recvProps The properties where we will store the received values.
    BlackoilPropsDataHandle(const Dune::CpGrid& sendGrid,
                            const Dune::CpGrid& recvGrid,
                            const BlackoilPropsAdFromDeck& sendProps,
                            BlackoilPropsAdFromDeck& recvProps)
        : sendGrid_(sendGrid), recvGrid_(recvGrid), sendProps_(sendProps), recvProps_(recvProps),
          size_(2)
    {
        // satOilMax might be non empty. In this case we will need to send it, too.
        if ( sendProps.satOilMax_.size()>0 )
        {
            recvProps_.satOilMax_.resize(recvGrid.numCells(),
                                         -std::numeric_limits<double>::max());
            ++size_;
        }
    }

    bool fixedsize(int /*dim*/, int /*codim*/)
    {
        return true;
    }

    template<class T>
    std::size_t size(const T&)
    {
        if ( T::codimension == 0)
        {
            // We only send pvtTableIdx_, cellPvtRegionIdx_, and maybe satOilMax_
            return size_;
        }
        else
        {
            OPM_THROW(std::logic_error, "Data handle can only be used for elements");
        }
    }
    template<class B, class T>
    void gather(B& buffer, const T& e)
    {
        assert( T::codimension == 0);

        buffer.write(sendProps_.cellPvtRegionIndex()[e.index()]);
        buffer.write(sendProps_.pvtTableIdx_[e.index()]);
        if ( size_==2 )
        {
            return;
        }
        buffer.write(sendProps_.satOilMax_[e.index()]);
    }
    template<class B, class T>
    void scatter(B& buffer, const T& e, std::size_t size)
    {
        assert( T::codimension == 0);
        assert( size==size_ ); (void) size;
        double val;
        buffer.read(val);
        recvProps_.cellPvtRegionIdx_[e.index()]=val;
        buffer.read(val);
        recvProps_.pvtTableIdx_[e.index()]=val;
        if ( size_==2 )
            return;
        buffer.read(val);
        recvProps_.satOilMax_[e.index()]=val;
    }
    bool contains(int dim, int codim)
    {
        return dim==3 && codim==0;
    }
private:
    /// \brief The grid that the data is attached to when sending
    const Dune::CpGrid& sendGrid_;
    /// \brief The grid that the data is attached to when receiving
    const Dune::CpGrid& recvGrid_;
    /// \brief The properties where we will retieve the values to be sent.
    const BlackoilPropsAdFromDeck& sendProps_;
    // \brief The properties where we will store the received values.
    BlackoilPropsAdFromDeck& recvProps_;
    std::size_t size_;
};

} // end namespace Opm
