#include "berkeley_gfx.hpp"
#include "renderer.hpp"
#include "pipelines.hpp"
#include "command_buffer.hpp"
#include "buffer.hpp"

#include <string>
#include <fstream>
#include <streambuf>

#include <glm/gtc/matrix_transform.hpp>

// Import the tinyGlTF library to load glTF models
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINYGLTF_USE_CPP14
#include "tiny_gltf.h"

using namespace BG;

// String for storing the shaders
std::string vertexShader;
std::string fragmentShader;

// Our vertex format
struct Vertex {
  glm::vec3 pos;
  glm::vec3 color;
};

// Our CPU side index / vertex buffer
std::vector<Vertex> vertices;
std::vector<uint32_t> indicies;

// Each draw cmd draws a child node of the glTF
// Which uses a subsection of the index buffer (firstIndex + indexCount)
// The index values are offsetted with an additional vertex offset.
// (e.g. index N means the vertexOffset + Nth element of the vertex buffer)
struct DrawCmd
{
  uint32_t indexCount;
  uint32_t firstIndex;
  uint32_t vertexOffset;
  glm::mat4 transform;
};

struct ShaderUniform
{
  glm::mat4 modelMtx;
  glm::mat4 viewProjMtx;
};

glm::mat4 viewMtx;
glm::mat4 projMtx;

// List of draw commands
std::vector<DrawCmd> drawObjects;

// Read the shaders into string from file
void load_shader_file()
{
  std::ifstream tf(SRC_DIR"/sample/1_glTFViewer/fragment.glsl");
  fragmentShader = std::string((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());

  std::ifstream tv(SRC_DIR"/sample/1_glTFViewer/vertex.glsl");
  vertexShader = std::string((std::istreambuf_iterator<char>(tv)), std::istreambuf_iterator<char>());
}

// Documents for the glTF format:
// https://raw.githubusercontent.com/KhronosGroup/glTF/master/specification/2.0/figures/gltfOverview-2.0.0b.png
// Here for sake of simplicity we just flatten the entire scene hierarchy to a vertex buffer, a index buffer, and a series of offsets
void load_gltf_node(tinygltf::Model& model, int nodeId, glm::mat4 transform)
{
  auto& node = model.nodes[nodeId];

  // Hierarchical transform
  glm::mat4 localTransform;
  
  localTransform = node.matrix.size() == 16 ? glm::mat4(
    node.matrix[     0], node.matrix[     1], node.matrix[     2], node.matrix[     3], 
    node.matrix[ 4 + 0], node.matrix[ 4 + 1], node.matrix[ 4 + 2], node.matrix[ 4 + 3],
    node.matrix[ 8 + 0], node.matrix[ 8 + 1], node.matrix[ 8 + 2], node.matrix[ 8 + 3],
    node.matrix[12 + 0], node.matrix[12 + 1], node.matrix[12 + 2], node.matrix[12 + 3]
  ) : glm::mat4(1.0);

  localTransform = localTransform * transform;

  // The node contains a mesh
  if (node.mesh >= 0)
  {
    auto& mesh = model.meshes[node.mesh];

    spdlog::info("======== NODE {} ========", nodeId);

    // Iterate through all primitives of the mesh
    for (auto& primitive : mesh.primitives)
    {
      // Get the vertex position accessor (and relavent buffers)
      auto& positionAccessor = model.accessors[primitive.attributes["POSITION"]];
      spdlog::info("{}x{}, offset = {}", positionAccessor.count, positionAccessor.ByteStride(model.bufferViews[positionAccessor.bufferView]), positionAccessor.byteOffset);
      
      auto& positionBufferView = model.bufferViews[positionAccessor.bufferView];
      auto& positionBuffer = model.buffers[positionBufferView.buffer];

      size_t positionBufferStride = positionAccessor.ByteStride(model.bufferViews[positionAccessor.bufferView]);

      // Get the index accessor (and relavent buffers)
      auto& indexAccessor = model.accessors[primitive.indices];
      spdlog::info("{}x{}, offset = {}", indexAccessor.count, indexAccessor.ByteStride(model.bufferViews[indexAccessor.bufferView]), indexAccessor.byteOffset);

      auto& indexBufferView = model.bufferViews[indexAccessor.bufferView];
      auto& indexBuffer = model.buffers[indexBufferView.buffer];

      size_t indexBufferStride = indexAccessor.ByteStride(model.bufferViews[indexAccessor.bufferView]);

      // Record the current size of our vertex buffer and index buffer
      // We will be pushing new data into these two vectors
      // The current size will be the starting offset of this draw command
      size_t startVertex = vertices.size();
      size_t startIndex = indicies.size();

      // Push all vertices
      for (size_t index = 0; index < positionAccessor.count; index++)
      {
        // Get the base address and store the vertex
        float* elementBase = (float*)((uint8_t*)(positionBuffer.data.data()) + positionBufferView.byteOffset + positionAccessor.byteOffset + positionBufferStride * index);
        Vertex v;
        v.pos = glm::vec3(elementBase[0], elementBase[1], elementBase[2]);
        v.color = glm::vec3(0.7);
        vertices.push_back(v);
      }

      // Push all indices
      for (size_t index = 0; index < indexAccessor.count; index++)
      {
        uint16_t* elementBase = (uint16_t*)((uint8_t*)(indexBuffer.data.data()) + indexBufferView.byteOffset + indexAccessor.byteOffset + indexBufferStride * index);
        indicies.push_back(uint32_t(elementBase[0]));
      }

      // Append a new draw command to the list
      drawObjects.push_back({
        uint32_t(indexAccessor.count),
        uint32_t(startIndex),
        uint32_t(startVertex),
        localTransform
        });
    }
  }

  // Recursively go through all child nodes
  for (int childNodeId : node.children)
  {
    load_gltf_node(model, childNodeId, localTransform);
  }
}

// The function that loads the glTF file and initiate the recursive call to flatten the hierarchy
void load_gltf_model()
{
  std::string model_file = SRC_DIR"/assets/glTF-Sample-Models/2.0/WaterBottle/glTF/WaterBottle.gltf";

  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  std::string err;
  std::string warn;

  // Here we are loadinga *.gltf file which is an ASCII json.
  // If we want to load *.glb (binary package), we should use the LoadBinaryFromFile function
  bool ret = loader.LoadASCIIFromFile(&model, &err, &warn, model_file);

  // Check whether the library successfully loaded the glTF model
  if (!warn.empty()) {
    spdlog::warn("Warn: %s\n", warn.c_str());
  }

  if (!err.empty()) {
    spdlog::error("Err: %s\n", err.c_str());
    throw std::runtime_error("glTF parsing error");
  }

  if (!ret) {
    spdlog::error("Failed to parse glTF");
    throw std::runtime_error("Fail to parse glTF");
  }

  // Get the default scene
  auto& scene = model.scenes[model.defaultScene];

  glm::mat4 transform = glm::mat4(1.0);

  // Initiate recursive call on the root nodes
  for (int sceneRootNodeId : scene.nodes)
  {
    load_gltf_node(model, sceneRootNodeId, transform);
  }

  spdlog::info("======== glTF load finished ========");
}

// Main function
int main(int, char**)
{
  // Load the shader file into string
  load_shader_file();

  // Load the glTF models into vertex and index buffer
  load_gltf_model();

  // Instantiate the Berkeley Gfx renderer & the backend
  Renderer r("Sample Project - glTF Viewer", true);

  Pipeline::InitBackend();

  std::shared_ptr<Pipeline> pipeline;

  // Our framebuffers (see triangle.cpp for explainations)
  std::vector<vk::UniqueFramebuffer> framebuffers;

  // Our GPU buffers holding the vertices and the indices
  std::shared_ptr<Buffer> vertexBuffer, indexBuffer, uniformBuffer;

  BG::VertexBufferBinding vertexBinding;

  r.Run(
    // Init
    [&]() {
      // Allocate a buffer on GPU, and flag it as a Vertex Buffer & can be copied towards
      vertexBuffer = r.getMemoryAllocator()->AllocCPU2GPU(vertices.size() * sizeof(Vertex), vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst);
      // Map the GPU buffer into the CPU's memory space
      Vertex* vertexBufferGPU = vertexBuffer->Map<Vertex>();
      // Copy our vertex list into GPU buffer
      std::copy(vertices.begin(), vertices.end(), vertexBufferGPU);
      // Unmap the GPU buffer
      vertexBuffer->UnMap();

      // Allocate a buffer on GPU, and flag it as a Index Buffer & can be copied towards
      indexBuffer = r.getMemoryAllocator()->AllocCPU2GPU(indicies.size() * sizeof(uint32_t), vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst);
      // Map the GPU buffer into the CPU's memory space
      uint32_t* indexBufferGPU = indexBuffer->Map<uint32_t>();
      // Copy our vertex list into GPU buffer
      std::copy(indicies.begin(), indicies.end(), indexBufferGPU);
      // Unmap the GPU buffer
      indexBuffer->UnMap();

      uniformBuffer = r.getMemoryAllocator()->AllocCPU2GPU(sizeof(ShaderUniform) * r.getSwapchainImageViews().size() * drawObjects.size(), vk::BufferUsageFlagBits::eUniformBuffer);

      // Create a empty pipline
      pipeline = r.CreatePipeline();
      // Add a vertex binding
      vertexBinding = pipeline->AddVertexBuffer<Vertex>();
      // Specify two vertex input attributes from the binding
      pipeline->AddAttribute(vertexBinding, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, pos));
      pipeline->AddAttribute(vertexBinding, 1, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, color));
      // Specify the uniform buffer binding
      pipeline->AddDescriptorUniform(0, vk::ShaderStageFlagBits::eVertex);
      // Add shaders
      pipeline->AddFragmentShaders(fragmentShader);
      pipeline->AddVertexShaders(vertexShader);
      // Set the viewport
      pipeline->SetViewport(float(r.getWidth()), float(r.getHeight()));
      // Add an attachment for the pipeline to render to
      pipeline->AddAttachment(r.getSwapChainFormat(), vk::ImageLayout::eUndefined, vk::ImageLayout::ePresentSrcKHR);
      // Build the pipeline
      pipeline->BuildPipeline();

      int width = r.getWidth(), height = r.getHeight();

      // Create a Framebuffer (specifications of the render targets) for each SwapChain image
      for (auto& imageView : r.getSwapchainImageViews())
      {
        std::vector<vk::ImageView> renderTarget{ imageView.get() };
        framebuffers.push_back(r.CreateFramebuffer(pipeline->GetRenderPass(), renderTarget, width, height));
      }
    },
    // Render
    [&](Renderer::Context& ctx) {
      int width = r.getWidth(), height = r.getHeight();

      viewMtx = glm::lookAt(glm::vec3(cos(ctx.time), cos(ctx.time * 0.5), sin(ctx.time)), glm::vec3(0.0), glm::vec3(0.0, 1.0, 0.0));
      projMtx = glm::perspective(glm::radians(45.0f), float(width) / float(height), 0.1f, 10.0f);
      projMtx[1][1] *= -1.0;

      ShaderUniform* uniformBufferGPU = uniformBuffer->Map<ShaderUniform>();
      int i = 0;
      for (auto& drawCmd : drawObjects)
      {
        auto& uniform = uniformBufferGPU[ctx.imageIndex * drawObjects.size() + i];
        uniform.modelMtx = drawCmd.transform;
        uniform.viewProjMtx = projMtx * viewMtx;
        i++;
      }
      uniformBuffer->UnMap();

      // Begin & resets the command buffer
      ctx.cmdBuffer.Begin();
      // Use the RenderPass from the pipeline we built
      ctx.cmdBuffer.WithRenderPass(*pipeline, framebuffers[ctx.imageIndex].get(), glm::uvec2(width, height), [&](){
        // Bind the pipeline to use
        ctx.cmdBuffer.BindPipeline(*pipeline);
        // Bind the vertex buffer
        ctx.cmdBuffer.BindVertexBuffer(vertexBinding, vertexBuffer, 0);
        // Bind the index buffer
        ctx.cmdBuffer.BindIndexBuffer(indexBuffer, 0);
        // Draw objects
        int i = 0;
        for (auto& drawCmd : drawObjects)
        {
          ctx.cmdBuffer.BindGraphicsUniformBuffer(*pipeline, ctx.descPool, uniformBuffer, sizeof(ShaderUniform) * (ctx.imageIndex * drawObjects.size() + i), sizeof(ShaderUniform), 0);
          ctx.cmdBuffer.DrawIndexed(drawCmd.indexCount, drawCmd.firstIndex, drawCmd.vertexOffset);
          i++;
        }
        });
      // End the recording of command buffer
      ctx.cmdBuffer.End();

      // After this callback ends, the renderer will submit the recorded commands, and present the image when it's rendered
    },
    // Cleanup
    [&]() {

    }
  );

  return 0;
}
