// DEFINE: %{option} = enable-runtime-library=true
// DEFINE: %{command} = mlir-opt %s --sparse-compiler=%{option} | \
// DEFINE: TENSOR0="%mlir_src_dir/test/Integration/data/test.mtx" \
// DEFINE: mlir-cpu-runner \
// DEFINE:  -e entry -entry-point-result=void  \
// DEFINE:  -shared-libs=%mlir_lib_dir/libmlir_c_runner_utils%shlibext,%mlir_lib_dir/libmlir_runner_utils%shlibext | \
// DEFINE: FileCheck %s
//
// RUN: %{command}
//
// Do the same run, but now with direct IR generation.
// REDEFINE: %{option} = enable-runtime-library=false
// RUN: %{command}

!Filename = !llvm.ptr<i8>

#DenseMatrix = #sparse_tensor.encoding<{
  dimLevelType = [ "dense", "dense" ],
  dimOrdering = affine_map<(i,j) -> (i,j)>
}>

#SparseMatrix = #sparse_tensor.encoding<{
  dimLevelType = [ "dense", "compressed" ],
  dimOrdering = affine_map<(i,j) -> (i,j)>
}>

#trait_assign = {
  indexing_maps = [
    affine_map<(i,j) -> (i,j)>, // A
    affine_map<(i,j) -> (i,j)>  // X (out)
  ],
  iterator_types = ["parallel", "parallel"],
  doc = "X(i,j) = A(i,j) * 2"
}

//
// Integration test that demonstrates assigning a sparse tensor
// to an all-dense annotated "sparse" tensor, which effectively
// result in inserting the nonzero elements into a linearized array.
//
// Note that there is a subtle difference between a non-annotated
// tensor and an all-dense annotated tensor. Both tensors are assumed
// dense, but the former remains an n-dimensional memref whereas the
// latter is linearized into a one-dimensional memref that is further
// lowered into a storage scheme that is backed by the runtime support
// library.
module {
  //
  // A kernel that assigns multiplied elements from A to X.
  //
  func.func @dense_output(%arga: tensor<?x?xf64, #SparseMatrix>) -> tensor<?x?xf64, #DenseMatrix> {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c2 = arith.constant 2.0 : f64
    %d0 = tensor.dim %arga, %c0 : tensor<?x?xf64, #SparseMatrix>
    %d1 = tensor.dim %arga, %c1 : tensor<?x?xf64, #SparseMatrix>
    %init = bufferization.alloc_tensor(%d0, %d1) : tensor<?x?xf64, #DenseMatrix>
    %0 = linalg.generic #trait_assign
       ins(%arga: tensor<?x?xf64, #SparseMatrix>)
      outs(%init: tensor<?x?xf64, #DenseMatrix>) {
      ^bb(%a: f64, %x: f64):
        %0 = arith.mulf %a, %c2 : f64
        linalg.yield %0 : f64
    } -> tensor<?x?xf64, #DenseMatrix>
    return %0 : tensor<?x?xf64, #DenseMatrix>
  }

  func.func private @getTensorFilename(index) -> (!Filename)
  func.func private @printMemref1dF64(%ptr : memref<?xf64>) attributes { llvm.emit_c_interface }

  //
  // Main driver that reads matrix from file and calls the kernel.
  //
  func.func @entry() {
    %d0 = arith.constant 0.0 : f64
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index

    // Read the sparse matrix from file, construct sparse storage.
    %fileName = call @getTensorFilename(%c0) : (index) -> (!Filename)
    %a = sparse_tensor.new %fileName
      : !Filename to tensor<?x?xf64, #SparseMatrix>

    // Call the kernel.
    %0 = call @dense_output(%a)
      : (tensor<?x?xf64, #SparseMatrix>) -> tensor<?x?xf64, #DenseMatrix>

    //
    // Print the linearized 5x5 result for verification.
    // CHECK: 25
    // CHECK: [2,  0,  0,  2.8,  0,  0,  4,  0,  0,  5,  0,  0,  6,  0,  0,  8.2,  0,  0,  8,  0,  0,  10.4,  0,  0,  10
    //
    %n = sparse_tensor.number_of_entries %0 : tensor<?x?xf64, #DenseMatrix>
    vector.print %n : index
    %m = sparse_tensor.values %0
      : tensor<?x?xf64, #DenseMatrix> to memref<?xf64>
    call @printMemref1dF64(%m) : (memref<?xf64>) -> ()

    // Release the resources.
    bufferization.dealloc_tensor %a : tensor<?x?xf64, #SparseMatrix>
    bufferization.dealloc_tensor %0 : tensor<?x?xf64, #DenseMatrix>

    return
  }
}
