// DEFINE: %{option} = enable-runtime-library=true
// DEFINE: %{command} = mlir-opt %s --sparse-compiler=%{option} | \
// DEFINE: TENSOR0="%mlir_src_dir/test/Integration/data/test.tns" \
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

#SparseTensor = #sparse_tensor.encoding<{
  dimLevelType = [ "compressed", "compressed", "compressed", "compressed",
                   "compressed", "compressed", "compressed", "compressed" ],
  // Note that any dimOrdering permutation should give the same results
  // since, even though it impacts the sparse storage scheme layout,
  // it should not change the semantics.
  dimOrdering = affine_map<(i,j,k,l,m,n,o,p) -> (p,o,j,k,i,l,m,n)>
}>

#trait_flatten = {
  indexing_maps = [
    affine_map<(i,j,k,l,m,n,o,p) -> (i,j,k,l,m,n,o,p)>, // A
    affine_map<(i,j,k,l,m,n,o,p) -> (i,j)>              // X (out)
  ],
  iterator_types = [ "parallel",  "parallel",  "reduction", "reduction",
                     "reduction", "reduction", "reduction", "reduction" ],
  doc = "X(i,j) += A(i,j,k,l,m,n,o,p)"
}

//
// Integration test that lowers a kernel annotated as sparse to
// actual sparse code, initializes a matching sparse storage scheme
// from file, and runs the resulting code with the JIT compiler.
//
module {
  //
  // A kernel that flattens a rank 8 tensor into a dense matrix.
  //
  func.func @kernel_flatten(%arga: tensor<7x3x3x3x3x3x5x3xf64, #SparseTensor>,
                            %argx: tensor<7x3xf64>)
		       -> tensor<7x3xf64> {
    %0 = linalg.generic #trait_flatten
      ins(%arga: tensor<7x3x3x3x3x3x5x3xf64, #SparseTensor>)
      outs(%argx: tensor<7x3xf64>) {
      ^bb(%a: f64, %x: f64):
        %0 = arith.addf %x, %a : f64
        linalg.yield %0 : f64
    } -> tensor<7x3xf64>
    return %0 : tensor<7x3xf64>
  }

  func.func private @getTensorFilename(index) -> (!Filename)
  func.func private @printMemrefF64(%ptr : tensor<*xf64>)

  //
  // Main driver that reads tensor from file and calls the sparse kernel.
  //
  func.func @entry() {
    %d0 = arith.constant 0.0 : f64
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c3 = arith.constant 3 : index
    %c7 = arith.constant 7 : index

    // Setup matrix memory that is initialized to zero.
    %x = arith.constant dense<0.000000e+00> : tensor<7x3xf64>

    // Read the sparse tensor from file, construct sparse storage.
    %fileName = call @getTensorFilename(%c0) : (index) -> (!Filename)
    %a = sparse_tensor.new %fileName : !Filename to tensor<7x3x3x3x3x3x5x3xf64, #SparseTensor>

    // Call the kernel.
    %0 = call @kernel_flatten(%a, %x)
      : (tensor<7x3x3x3x3x3x5x3xf64, #SparseTensor>, tensor<7x3xf64>) -> tensor<7x3xf64>

    // Print the result for verification.
    //
    // CHECK:      {{\[}}[6.25,   0,   0],
    // CHECK-NEXT: [4.224,   6.21,   0],
    // CHECK-NEXT: [0,   0,   15.455],
    // CHECK-NEXT: [0,   0,   0],
    // CHECK-NEXT: [0,   0,   0],
    // CHECK-NEXT: [0,   0,   0],
    // CHECK-NEXT: [7,   0,   0]]
    //
    %1 = tensor.cast %0 : tensor<7x3xf64> to tensor<*xf64>
    call @printMemrefF64(%1) : (tensor<*xf64>) -> ()

    // Release the resources.
    bufferization.dealloc_tensor %a : tensor<7x3x3x3x3x3x5x3xf64, #SparseTensor>

    return
  }
}
