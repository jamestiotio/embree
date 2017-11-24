// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "scene_bezier_curves.h"
#include "scene.h"

namespace embree
{
#if defined(EMBREE_LOWEST_ISA)

  NativeCurves::NativeCurves (Device* device, RTCIntersectMode subtype, RTCCurveBasis basis) 
    : Geometry(device,BEZIER_CURVES,0,1), subtype(subtype), basis(basis), tessellationRate(4)
  {
    vertices.resize(numTimeSteps);
  }

  void NativeCurves::enabling() 
  {
    if (numTimeSteps == 1) scene->world.numBezierCurves += numPrimitives; 
    else                   scene->worldMB.numBezierCurves += numPrimitives; 
  }
  
  void NativeCurves::disabling() 
  {
    if (numTimeSteps == 1) scene->world.numBezierCurves -= numPrimitives; 
    else                   scene->worldMB.numBezierCurves -= numPrimitives;
  }
  
  void NativeCurves::setMask (unsigned mask) 
  {
    this->mask = mask; 
    Geometry::update();
  }

  void NativeCurves::setIntersectMode(RTCIntersectMode type_in)
  {
    this->subtype = type_in;
    Geometry::update();
  }
  
  void* NativeCurves::newBuffer(RTCBufferType type, size_t stride, unsigned int size) 
  { 
    /* verify that all accesses are 4 bytes aligned */
    if (stride & 0x3) 
      throw_RTCError(RTC_ERROR_INVALID_OPERATION,"data must be 4 bytes aligned");

    unsigned bid = type & 0xFFFF;
    if (type >= RTC_VERTEX_BUFFER0 && type < RTC_VERTEX_BUFFER_(RTC_MAX_TIME_STEPS)) 
    {
      if (bid >= vertices.size()) vertices.resize(bid+1);
      vertices[bid].newBuffer(device,size,stride);
      setNumTimeSteps((unsigned int)vertices.size());
      return vertices[bid].get();
    }
    else if (type >= RTC_USER_VERTEX_BUFFER0 && type < RTC_USER_VERTEX_BUFFER_(RTC_MAX_USER_VERTEX_BUFFERS))
    {
      if (bid >= userbuffers.size()) userbuffers.resize(bid+1);
      userbuffers[bid] = Buffer<char>(device,size,stride,true);
      return userbuffers[bid].get();
    }
    else if (type == RTC_INDEX_BUFFER) 
    {
      curves.newBuffer(device,size,stride); 
      setNumPrimitives(size);
      return curves.get();
    }
    else 
        throw_RTCError(RTC_ERROR_INVALID_ARGUMENT,"unknown buffer type");

    return nullptr;
  }
  
  void NativeCurves::setBuffer(RTCBufferType type, void* ptr, size_t offset, size_t stride, unsigned int size) 
  { 
    /* verify that all accesses are 4 bytes aligned */
    if (((size_t(ptr) + offset) & 0x3) || (stride & 0x3)) 
      throw_RTCError(RTC_ERROR_INVALID_OPERATION,"data must be 4 bytes aligned");

    unsigned bid = type & 0xFFFF;
    if (type >= RTC_VERTEX_BUFFER0 && type < RTC_VERTEX_BUFFER_(RTC_MAX_TIME_STEPS)) 
    {
      if (bid >= vertices.size()) vertices.resize(bid+1);
      vertices[bid].set(device,ptr,offset,stride,size); 
      vertices[bid].checkPadding16();
      //while (vertices.size() > 1 && vertices.back().getPtr() == nullptr)
      //  vertices.pop_back();
      setNumTimeSteps((unsigned int)vertices.size());
    }
    else if (type >= RTC_USER_VERTEX_BUFFER0 && type < RTC_USER_VERTEX_BUFFER0+RTC_MAX_USER_VERTEX_BUFFERS)
    {
      if (bid >= userbuffers.size()) userbuffers.resize(bid+1);
      userbuffers[bid] = Buffer<char>(device,size,stride);
      userbuffers[bid].set(device,ptr,offset,stride,size);  
      userbuffers[bid].checkPadding16();
    }
    else if (type == RTC_INDEX_BUFFER) 
    {
      curves.set(device,ptr,offset,stride,size); 
      setNumPrimitives(size);
    }
    else 
        throw_RTCError(RTC_ERROR_INVALID_ARGUMENT,"unknown buffer type"); 
  }

  void* NativeCurves::getBuffer(RTCBufferType type) 
  {
    if (type == RTC_INDEX_BUFFER) {
      return curves.get();
    }
    else if (type >= RTC_VERTEX_BUFFER0 && type < RTCBufferType(RTC_VERTEX_BUFFER0 + numTimeSteps)) {
      return vertices[type - RTC_VERTEX_BUFFER0].get();
    }
    else {
      throw_RTCError(RTC_ERROR_INVALID_ARGUMENT,"unknown buffer type"); 
      return nullptr;
    }
  }

  void NativeCurves::setTessellationRate(float N)
  {
    tessellationRate = clamp((int)N,1,16);
  }

  bool NativeCurves::verify () 
  {
    /*! verify consistent size of vertex arrays */
    if (vertices.size() == 0) return false;
    for (const auto& buffer : vertices)
      if (vertices[0].size() != buffer.size())
        return false;

    /*! verify indices */
    for (unsigned int i=0; i<numPrimitives; i++) {
      if (curves[i]+3 >= numVertices()) return false;
    }
    
    /*! verify vertices */
    for (const auto& buffer : vertices) {
      for (size_t i=0; i<buffer.size(); i++) {
	if (!isvalid(buffer[i].x)) return false;
        if (!isvalid(buffer[i].y)) return false;
        if (!isvalid(buffer[i].z)) return false;
        if (!isvalid(buffer[i].w)) return false;
      }
    }
    return true;
  }

  void NativeCurves::preCommit()
  {
    if (!isEnabled()) return;

    native_curves = (BufferView<unsigned>) curves;
    if (native_vertices.size() != vertices.size())
      native_vertices.resize(vertices.size());

    native_vertices0 = vertices[0];
    for (size_t i=0; i<vertices.size(); i++)
      native_vertices[i] = (BufferView<Vec3fa>) vertices[i];
  }

#endif

  namespace isa
  {
    template<typename Curve>
    __forceinline void NativeCurvesISA::interpolate_helper(unsigned primID, float u, float v, RTCBufferType buffer, 
                                                           float* P, float* dPdu, float* dPdv, float* ddPdudu, float* ddPdvdv, float* ddPdudv, unsigned int numFloats) 
    {
      /* calculate base pointer and stride */
      assert((buffer >= RTC_VERTEX_BUFFER0 && buffer < RTCBufferType(RTC_VERTEX_BUFFER0 + numTimeSteps)) ||
             (buffer >= RTC_USER_VERTEX_BUFFER0 && buffer <= RTC_USER_VERTEX_BUFFER1));
      const char* src = nullptr; 
      size_t stride = 0;
      if (buffer >= RTC_USER_VERTEX_BUFFER0) {
        src    = userbuffers[buffer&0xFFFF].getPtr();
        stride = userbuffers[buffer&0xFFFF].getStride();
      } else {
        src    = vertices[buffer&0xFFFF].getPtr();
        stride = vertices[buffer&0xFFFF].getStride();
      }
      
      for (unsigned int i=0; i<numFloats; i+=VSIZEX)
      {
        size_t ofs = i*sizeof(float);
        const size_t curve = curves[primID];
        const vboolx valid = vintx((int)i)+vintx(step) < vintx((int)numFloats);
        const vfloatx p0 = vfloatx::loadu(valid,(float*)&src[(curve+0)*stride+ofs]);
        const vfloatx p1 = vfloatx::loadu(valid,(float*)&src[(curve+1)*stride+ofs]);
        const vfloatx p2 = vfloatx::loadu(valid,(float*)&src[(curve+2)*stride+ofs]);
        const vfloatx p3 = vfloatx::loadu(valid,(float*)&src[(curve+3)*stride+ofs]);
        
        const Curve bezier(p0,p1,p2,p3);
        if (P      ) vfloatx::storeu(valid,P+i,      bezier.eval(u));
        if (dPdu   ) vfloatx::storeu(valid,dPdu+i,   bezier.eval_du(u));
        if (ddPdudu) vfloatx::storeu(valid,ddPdudu+i,bezier.eval_dudu(u));
      }
    }
    
    template<typename InputCurve3fa, typename OutputCurve3fa>
    void NativeCurvesISA::commit_helper()
    {
      if (native_curves.size() != size()) 
      {
        native_curves = Buffer<unsigned>(device,size(),sizeof(unsigned int),true);
        parallel_for(size_t(0), size(), size_t(1024), [&] ( const range<size_t> r) {
            for (size_t i=r.begin(); i<r.end(); i++) {
              if (curves[i]+3 >= numVertices()) native_curves[i] = 0xFFFFFFF0; // invalid curves stay invalid this way
              else                              native_curves[i] = unsigned(4*i);
            }
          });
      }
      
      if (native_vertices.size() != vertices.size())
        native_vertices.resize(vertices.size());
      
      parallel_for(vertices.size(), [&] ( const size_t i ) {
          
          if (native_vertices[i].size() != 4*size())
            native_vertices[i] = Buffer<Vec3fa>(device,4*size(),sizeof(Vec3fa),true);
          
          parallel_for(size_t(0), size(), size_t(1024), [&] ( const range<size_t> rj ) {
              
              for (size_t j=rj.begin(); j<rj.end(); j++)
              {
                const unsigned id = curves[j];
                if (id+3 >= numVertices()) continue; // ignore invalid curves
                const Vec3fa v0 = vertices[i][id+0];
                const Vec3fa v1 = vertices[i][id+1];
                const Vec3fa v2 = vertices[i][id+2];
                const Vec3fa v3 = vertices[i][id+3];
                const InputCurve3fa icurve(v0,v1,v2,v3);
                OutputCurve3fa ocurve; convert<Vec3fa>(icurve,ocurve);
                native_vertices[i].store(4*j+0,ocurve.v0);
                native_vertices[i].store(4*j+1,ocurve.v1);
                native_vertices[i].store(4*j+2,ocurve.v2);
                native_vertices[i].store(4*j+3,ocurve.v3);
              }
            });
        });
      native_vertices0 = native_vertices[0];
    }
    
    NativeCurves* createCurvesBezier(Device* device, RTCIntersectMode subtype, RTCCurveBasis basis) {
      return new CurvesBezier(device,subtype,basis);
    }
    
    void CurvesBezier::preCommit() {
#if defined(EMBREE_NATIVE_CURVE_BSPLINE)
      if (isEnabled()) commit_helper<BezierCurve3fa,BSplineCurve3fa>();
#else
      NativeCurves::preCommit();
#endif
      Geometry::preCommit();
    }
    
    void CurvesBezier::interpolate(unsigned primID, float u, float v, RTCBufferType buffer, 
                                   float* P, float* dPdu, float* dPdv, float* ddPdudu, float* ddPdvdv, float* ddPdudv, unsigned int numFloats) 
    {
      interpolate_helper<BezierCurveT<vfloatx>>(primID,u,v,buffer,P,dPdu,dPdv,ddPdudu,ddPdvdv,ddPdudv,numFloats);
    }
    
    NativeCurves* createCurvesBSpline(Device* device, RTCIntersectMode subtype, RTCCurveBasis basis) {
      return new CurvesBSpline(device,subtype,basis);
    }
    
    void CurvesBSpline::preCommit() {
#if defined(EMBREE_NATIVE_CURVE_BSPLINE)
      NativeCurves::preCommit();
#else
      if (isEnabled()) commit_helper<BSplineCurve3fa,BezierCurve3fa>();
#endif
      Geometry::preCommit();
    }
    
    void CurvesBSpline::interpolate(unsigned primID, float u, float v, RTCBufferType buffer, 
                                    float* P, float* dPdu, float* dPdv, float* ddPdudu, float* ddPdvdv, float* ddPdudv, unsigned int numFloats) 
    {
      interpolate_helper<BSplineCurveT<vfloatx>>(primID,u,v,buffer,P,dPdu,dPdv,ddPdudu,ddPdvdv,ddPdudv,numFloats);
    }
  }
}
