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

// TODO: 
//       - adjust parallel build thresholds
//       - openNodesBasedOnExtend should consider max extended size
  
#pragma once

#include "heuristic_binning.h"
#include "heuristic_spatial.h"

/* stop opening of all bref.geomIDs are the same */
#define EQUAL_GEOMID_STOP_CRITERIA 1

/* 10% spatial extend threshold */
#define MAX_EXTEND_THRESHOLD   0.1f

/* maximum is 8 children */
#define MAX_OPENED_CHILD_NODES 8

/* open until all build refs are below threshold size in one step */
#define USE_LOOP_OPENING 0

namespace embree
{
  namespace isa
  { 
    /*! Performs standard object binning */
    template<typename NodeOpenerFunc, typename PrimRef, size_t OBJECT_BINS>
      struct HeuristicArrayOpenMergeSAH
      {
        typedef BinSplit<OBJECT_BINS> Split;
        typedef BinInfoT<OBJECT_BINS,PrimRef,BBox3fa> Binner;
        
        static const size_t PARALLEL_THRESHOLD = 1024;
        static const size_t PARALLEL_FIND_BLOCK_SIZE = 512;
        static const size_t PARALLEL_PARTITION_BLOCK_SIZE = 128;

        static const size_t MOVE_STEP_SIZE = 64;
        static const size_t CREATE_SPLITS_STEP_SIZE = 128;

        __forceinline HeuristicArrayOpenMergeSAH ()
          : prims0(nullptr) {}
        
        /*! remember prim array */
        __forceinline HeuristicArrayOpenMergeSAH (const NodeOpenerFunc& nodeOpenerFunc, PrimRef* prims0)
          : prims0(prims0), nodeOpenerFunc(nodeOpenerFunc) {}

        /*! compute extended ranges */
        __forceinline void setExtentedRanges(const PrimInfoExtRange& set, PrimInfoExtRange& lset, PrimInfoExtRange& rset, const size_t lweight, const size_t rweight)
        {
          assert(set.ext_range_size() > 0);
          const float left_factor           = (float)lweight / (lweight + rweight);
          const size_t ext_range_size       = set.ext_range_size();
          const size_t left_ext_range_size  = min((size_t)(floorf(left_factor * ext_range_size)),ext_range_size);
          const size_t right_ext_range_size = ext_range_size - left_ext_range_size;
          lset.set_ext_range(lset.end() + left_ext_range_size);
          rset.set_ext_range(rset.end() + right_ext_range_size);
        }

        /*! move ranges */
        __forceinline void moveExtentedRange(const PrimInfoExtRange& set, const PrimInfoExtRange& lset, PrimInfoExtRange& rset)
        {
          const size_t left_ext_range_size = lset.ext_range_size();
          const size_t right_size = rset.size();

          /* has the left child an extended range? */
          if (left_ext_range_size > 0)
          {
            /* left extended range smaller than right range ? */
            if (left_ext_range_size < right_size)
            {
              /* only move a small part of the beginning of the right range to the end */
              parallel_for( rset.begin(), rset.begin()+left_ext_range_size, MOVE_STEP_SIZE, [&](const range<size_t>& r) {                  
                  for (size_t i=r.begin(); i<r.end(); i++)
                    prims0[i+right_size] = prims0[i];
                });
            }
            else
            {
              /* no overlap, move entire right range to new location, can be made fully parallel */
              parallel_for( rset.begin(), rset.end(), MOVE_STEP_SIZE,  [&](const range<size_t>& r) {
                  for (size_t i=r.begin(); i<r.end(); i++)
                    prims0[i+left_ext_range_size] = prims0[i];
                });
            }
            /* update right range */
            assert(rset.ext_end() + left_ext_range_size == set.ext_end());
            rset.move_right(left_ext_range_size);
          }
        }

        // ==========================================================================
        // ==========================================================================
        // ==========================================================================

        __noinline std::pair<size_t,bool> getProperties(const PrimInfoExtRange& set)
        {
          const Vec3fa diag = set.geomBounds.size();
          const size_t dim = maxDim(diag);
          assert(diag[dim] > 0.0f);
          const float inv_max_extend = 1.0f / diag[dim];
          const unsigned int geomID = prims0[set.begin()].geomID();
          
          auto body = [&] (const range<size_t>& r) -> std::pair<size_t,bool> { 
            bool commonGeomID = true;
            size_t opens = 0;
            for (size_t i=r.begin(); i<r.end(); i++) {
              commonGeomID &= prims0[i].geomID() == geomID; 
              if (!prims0[i].node.isLeaf() && prims0[i].bounds().size()[dim] * inv_max_extend > MAX_EXTEND_THRESHOLD) 
                opens += prims0[i].node.getN()-1; // coarse approximation
            }
            return std::pair<size_t,bool>(opens,commonGeomID); 
          };
          auto reduction = [&] (const std::pair<size_t,bool>& b0, const std::pair<size_t,bool>& b1) -> std::pair<size_t,bool> { 
            return std::pair<size_t,bool>(b0.first+b1.first,b0.second && b1.second); 
          };
          return parallel_reduce(set.begin(),set.end(),PARALLEL_FIND_BLOCK_SIZE,PARALLEL_THRESHOLD,std::pair<size_t,bool>(0,true),body,reduction);
        }

        //FIXME: should consider maximum available extended size 
        __noinline size_t openNodesBasedOnExtend(PrimInfoExtRange& set)
        {
          const Vec3fa diag = set.geomBounds.size();
          const size_t dim = maxDim(diag);
          assert(diag[dim] > 0.0f);
          const float inv_max_extend = 1.0f / diag[dim];
          const size_t ext_range_start = set.end();

          if (set.size() < PARALLEL_THRESHOLD) {
            size_t extra_elements = 0;
            for (size_t i=set.begin(); i<set.end(); i++)
            {
              if (!prims0[i].node.isLeaf() && prims0[i].bounds().size()[dim] * inv_max_extend > MAX_EXTEND_THRESHOLD)
              {
                PrimRef tmp[MAX_OPENED_CHILD_NODES];
                const size_t n = nodeOpenerFunc(prims0[i],tmp);
                assert(extra_elements + n-1 <= set.ext_range_size());
                for (size_t j=0;j<n;j++)
                  set.extend(tmp[j].bounds());
                  
                prims0[i] = tmp[0];
                for (size_t j=1;j<n;j++)
                  prims0[ext_range_start+extra_elements+j-1] = tmp[j]; 
                extra_elements += n-1;
              }
            }
            return extra_elements;
          }
          else {
            std::atomic<size_t> ext_elements;
            ext_elements.store(0);
            PrimInfo info = parallel_reduce( set.begin(), set.end(), CREATE_SPLITS_STEP_SIZE, PrimInfo(empty), [&](const range<size_t>& r) -> PrimInfo {
                PrimInfo info(empty);
                for (size_t i=r.begin();i<r.end();i++)
                  if (!prims0[i].node.isLeaf() && prims0[i].bounds().size()[dim] * inv_max_extend > MAX_EXTEND_THRESHOLD)
                  {
                    PrimRef tmp[MAX_OPENED_CHILD_NODES];
                    const size_t n = nodeOpenerFunc(prims0[i],tmp);
                    const size_t ID = ext_elements.fetch_add(n-1);
                    assert(ID + n-1 <= set.ext_range_size());

                    for (size_t j=0;j<n;j++)
                      info.extend(tmp[j].bounds());

                    prims0[i] = tmp[0];
                    for (size_t j=1;j<n;j++)
                      prims0[ext_range_start+ID+j-1] = tmp[j]; 
                  }
                return info;
              }, [] (const PrimInfo& a, const PrimInfo& b) { return PrimInfo::merge(a,b); });
            set.centBounds.extend(info.centBounds);
            assert(ext_elements.load() <= set.ext_range_size());
            return ext_elements.load();
          }
        } 


        __noinline size_t openNodesUntilSetIsFull(PrimInfoExtRange& set, const float threshold = MAX_EXTEND_THRESHOLD)
        {
          vfloat4 smallest_extend = pos_inf;
          for (size_t i=set.begin();i<set.end();i++)            
            smallest_extend = min(smallest_extend,(vfloat4)prims0[i].bounds().size());
          const vbool4 mask = smallest_extend > 0.0f;

          size_t extra_elements = 0;
          const size_t ext_range_start = set.end();
          while(set.has_ext_range())
          //for (size_t k=0;k<2;k++)
          {
            const size_t current_end = set.end()+extra_elements;
            for (size_t i=set.begin();i<current_end;i++)
              if (!prims0[i].node.isLeaf() && any(((vfloat4)prims0[i].bounds().size() > smallest_extend) & mask))
              {
                PrimRef tmp[MAX_OPENED_CHILD_NODES];
                const size_t n = nodeOpenerFunc(prims0[i],tmp);
                if(unlikely(extra_elements + n-1 > set.ext_range_size())) break; 

                for (size_t j=0;j<n;j++) set.extend(tmp[j].bounds());
                
                prims0[i] = tmp[0];
                for (size_t j=1;j<n;j++)
                  prims0[ext_range_start+extra_elements+j-1] = tmp[j]; 
                extra_elements += n-1;            
              }

            //smallest_extend *= 1.1f;

            if (unlikely(set.end()+extra_elements == current_end)) break;
          }
          
          assert(extra_elements <= set.ext_range_size());
          return extra_elements;
        }                 


        __noinline void openNodesBasedOnExtendLoop(PrimInfoExtRange& set, const size_t est_new_elements)
        {
          const Vec3fa diag = set.geomBounds.size();
          const size_t dim = maxDim(diag);
          assert(diag[dim] > 0.0f);
          const float inv_max_extend = 1.0f / diag[dim];
          size_t next_iteration_extra_elements = est_new_elements;          
          float threshold = MAX_EXTEND_THRESHOLD;
          while(next_iteration_extra_elements <= set.ext_range_size()) 
          {
            next_iteration_extra_elements = 0;
            size_t extra_elements = 0;
            const size_t ext_range_start = set.end();

            for (size_t i=set.begin(); i<set.end(); i++)
            {
              if (!prims0[i].node.isLeaf() && prims0[i].bounds().size()[dim] * inv_max_extend > threshold)
              {
                PrimRef tmp[MAX_OPENED_CHILD_NODES];
                const size_t n = nodeOpenerFunc(prims0[i],tmp);
                assert(extra_elements + n-1 <= set.ext_range_size());
                for (size_t j=0;j<n;j++)
                  set.extend(tmp[j].bounds());
                  
                prims0[i] = tmp[0];
                for (size_t j=1;j<n;j++)
                  prims0[ext_range_start+extra_elements+j-1] = tmp[j]; 
                extra_elements += n-1;

                for (size_t j=0;j<n;j++)
                  if (!tmp[j].node.isLeaf() && tmp[j].bounds().size()[dim] * inv_max_extend > MAX_EXTEND_THRESHOLD)
                    next_iteration_extra_elements += tmp[j].node.getN()-1; // coarse approximation

              }
            }
            assert( extra_elements <= set.ext_range_size());
            set._end += extra_elements;

            for (size_t i=set.begin();i<set.end();i++)
              assert(prims0[i].numPrimitives() > 0);

            if (unlikely(next_iteration_extra_elements == 0)) break;
            //threshold *= 4.0f;
          }
        } 

        // ==========================================================================
        // ==========================================================================
        // ==========================================================================
        
        
        __noinline const Split find(PrimInfoExtRange& set, const size_t logBlockSize)
        {
          /* single element */
          if (set.size() <= 1)
            return Split();

          /* disable opening if there is no overlap */
          const size_t D = 4;
          if (unlikely(set.has_ext_range() && set.size() <= D))
          {
            bool disjoint = true;
            for (size_t j=set.begin(); j<set.end()-1; j++) {
              for (size_t i=set.begin()+1; i<set.end(); i++) {
                if (conjoint(prims0[j].bounds(),prims0[i].bounds())) { 
                  disjoint = false; break; 
                }
              }
            }
            if (disjoint) set.set_ext_range(set.end()); /* disables opening */
          }

          std::pair<float,bool> p(0.0f,false);

          /* common geomID */
          if (unlikely(set.has_ext_range()))
          {
            p =  getProperties(set);
#if EQUAL_GEOMID_STOP_CRITERIA == 1
            const bool commonGeomID       = p.second;
            if (commonGeomID)
              set.set_ext_range(set.end()); /* disable opening */
#endif         
          }
          if (unlikely(set.has_ext_range()))
          {
            const size_t est_new_elements = p.first;
#if USE_LOOP_OPENING == 1
            openNodesBasedOnExtendLoop(set,est_new_elements);
#else
            const size_t max_ext_range_size = set.ext_range_size();
            size_t extra_elements = 0;

            if (est_new_elements <= max_ext_range_size)
              extra_elements = openNodesBasedOnExtend(set);

            set._end += extra_elements;
#endif
            if (set.ext_range_size() <= 1) set.set_ext_range(set.end()); /* disable opening */
          }
                    
          /* find best split */
          return object_find(set,logBlockSize);
        }


        /*! finds the best object split */
        __forceinline const Split object_find(const PrimInfoExtRange& set,const size_t logBlockSize)
        {
          if (set.size() < PARALLEL_THRESHOLD) return sequential_object_find(set,logBlockSize);
          else                                 return parallel_object_find  (set,logBlockSize);
        }

        /*! finds the best object split */
        __noinline const Split sequential_object_find(const PrimInfoExtRange& set, const size_t logBlockSize)
        {
          Binner binner(empty); 
          const BinMapping<OBJECT_BINS> mapping(set.centBounds,OBJECT_BINS);
          binner.bin(prims0,set.begin(),set.end(),mapping);
          Split s = binner.best(mapping,logBlockSize);
          SplitInfo info;
          binner.getSplitInfo(mapping, s, info);
          return s;
        }

        /*! finds the best split */
        __noinline const Split parallel_object_find(const PrimInfoExtRange& set, const size_t logBlockSize)
        {
          Binner binner(empty);
          const BinMapping<OBJECT_BINS> mapping(set.centBounds,OBJECT_BINS);
          const BinMapping<OBJECT_BINS>& _mapping = mapping; // CLANG 3.4 parser bug workaround
          binner = parallel_reduce(set.begin(),set.end(),PARALLEL_FIND_BLOCK_SIZE,binner,
                                   [&] (const range<size_t>& r) -> Binner { Binner binner(empty); 
                                     binner.bin(prims0+r.begin(),r.size(),_mapping); 
                                     return binner; },
                                   [&] (const Binner& b0, const Binner& b1) -> Binner { Binner r = b0; r.merge(b1,_mapping.size()); return r; });
          Split s = binner.best(mapping,logBlockSize);
          SplitInfo info;
          binner.getSplitInfo(mapping, s, info);
          return s;
        }
        
        /*! array partitioning */
        __noinline void split(const Split& split, const PrimInfoExtRange& set_i, PrimInfoExtRange& lset, PrimInfoExtRange& rset) 
        {
          PrimInfoExtRange set = set_i;

          /* valid split */
          if (unlikely(!split.valid())) {
            deterministic_order(set);
            splitFallback(set,lset,rset);
            return;
          }

          std::pair<size_t,size_t> ext_weights(0,0);

          /* object split */
          if (likely(set.size() < PARALLEL_THRESHOLD)) 
            ext_weights = sequential_object_split(split,set,lset,rset);
          else
            ext_weights = parallel_object_split(split,set,lset,rset);

          /* if we have an extended range, set extended child ranges and move right split range */
          if (unlikely(set.has_ext_range())) 
          {
            setExtentedRanges(set,lset,rset,ext_weights.first,ext_weights.second);
            moveExtentedRange(set,lset,rset);
          }
        }

        /*! array partitioning */
        std::pair<size_t,size_t> sequential_object_split(const Split& split, const PrimInfoExtRange& set, PrimInfoExtRange& lset, PrimInfoExtRange& rset) 
        {
          const size_t begin = set.begin();
          const size_t end   = set.end();
          PrimInfo local_left(empty);
          PrimInfo local_right(empty);
          const unsigned int splitPos = split.pos;
          const unsigned int splitDim = split.dim;
          const unsigned int splitDimMask = (unsigned int)1 << splitDim; 

          const vint4 vSplitPos(splitPos);
          const vbool4 vSplitMask( (int)splitDimMask );

          size_t center = serial_partitioning(prims0,
                                              begin,end,local_left,local_right,
                                              [&] (const PrimRef& ref) { 
                                                return split.mapping.bin_unsafe(ref,vSplitPos,vSplitMask);
                                              },
                                              [] (PrimInfo& pinfo,const PrimRef& ref) { pinfo.add(ref.bounds()); });          
          
          const size_t left_weight  = local_left.end;
          const size_t right_weight = local_right.end;
          new (&lset) PrimInfoExtRange(begin,center,center,local_left.geomBounds,local_left.centBounds);
          new (&rset) PrimInfoExtRange(center,end,end,local_right.geomBounds,local_right.centBounds);
          assert(area(lset.geomBounds) >= 0.0f);
          assert(area(rset.geomBounds) >= 0.0f);
          return std::pair<size_t,size_t>(left_weight,right_weight);
        }

        /*! array partitioning */
        __noinline std::pair<size_t,size_t> parallel_object_split(const Split& split, const PrimInfoExtRange& set, PrimInfoExtRange& lset, PrimInfoExtRange& rset)
        {
          const size_t begin = set.begin();
          const size_t end   = set.end();
          PrimInfo left; left.reset();
          PrimInfo right; right.reset();
          const unsigned int splitPos = split.pos;
          const unsigned int splitDim = split.dim;
          const unsigned int splitDimMask = (unsigned int)1 << splitDim;

          const vint4 vSplitPos(splitPos);
          const vbool4 vSplitMask( (int)splitDimMask );
          auto isLeft = [&] (const PrimRef& ref) { return split.mapping.bin_unsafe(ref,vSplitPos,vSplitMask); };

          const size_t center = parallel_partitioning(
            prims0,begin,end,EmptyTy(),left,right,isLeft,
            [] (PrimInfo& pinfo,const PrimRef& ref) { pinfo.add(ref.bounds()); },
            [] (PrimInfo& pinfo0,const PrimInfo& pinfo1) { pinfo0.merge(pinfo1); },
            PARALLEL_PARTITION_BLOCK_SIZE);

          const size_t left_weight  = left.end;
          const size_t right_weight = right.end;
          
          new (&lset) PrimInfoExtRange(begin,center,center,left.geomBounds,left.centBounds);
          new (&rset) PrimInfoExtRange(center,end,end,right.geomBounds,right.centBounds);
          assert(area(lset.geomBounds) >= 0.0f);
          assert(area(rset.geomBounds) >= 0.0f);

          return std::pair<size_t,size_t>(left_weight,right_weight);
        }

        void deterministic_order(const extended_range<size_t>& set) 
        {
          /* required as parallel partition destroys original primitive order */
          std::sort(&prims0[set.begin()],&prims0[set.end()]);
        }

        void splitFallback(const PrimInfoExtRange& set, PrimInfoExtRange& lset, PrimInfoExtRange& rset)
        {
          const size_t begin = set.begin();
          const size_t end   = set.end();
          const size_t center = (begin + end)/2;

          PrimInfo left(empty);
          for (size_t i=begin; i<center; i++)
            left.add(prims0[i].bounds());

          const size_t lweight = left.end;
          
          PrimInfo right(empty);
          for (size_t i=center; i<end; i++)
            right.add(prims0[i].bounds());	

          const size_t rweight = right.end;

          new (&lset) PrimInfoExtRange(begin,center,center,left.geomBounds,left.centBounds);
          new (&rset) PrimInfoExtRange(center,end,end,right.geomBounds,right.centBounds);

          /* if we have an extended range */
          if (set.has_ext_range()) 
          {
            setExtentedRanges(set,lset,rset,lweight,rweight);
            moveExtentedRange(set,lset,rset);
          }
        }
        
      private:
        PrimRef* const prims0;
        const NodeOpenerFunc& nodeOpenerFunc;
      };
  }
}
