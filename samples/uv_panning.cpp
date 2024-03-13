#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <fstream>
#include <iostream>

#include <getopt/getopt.h>

#include <utils/Path.h>

#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/Camera.h>
#include <filament/View.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Skybox.h>


#include <math/mat3.h>
#include <math/mat4.h>
#include <math/vec4.h>

#include <filamentapp/Config.h>
#include <filamentapp/FilamentApp.h>

#include <stb_image.h>

#include <utils/EntityManager.h>

#include <filamat/MaterialBuilder.h>
#include <filameshio/MeshReader.h>

#include <image/LinearImage.h>
#include <imageio/ImageDecoder.h>


using namespace filament::math;
using namespace filament;
using namespace filamesh;
using namespace filamat;
using namespace utils;
using namespace image;

static std::vector<Path> g_filenames;

static MeshReader::MaterialRegistry g_materialInstances;
static std::vector<MeshReader::Mesh> g_meshes;


static Config g_config;

struct Vertex {
    filament::math::float2 position;
    filament::math::float2 uv;
};



static void cleanup(Engine* engine, View* view, Scene* scene) {
    std::vector<filament::MaterialInstance*> materialList(g_materialInstances.numRegistered());
    g_materialInstances.getRegisteredMaterials(materialList.data());
    for (auto material : materialList) {
        engine->destroy(material);
    }
    g_materialInstances.unregisterAll();
    EntityManager& em = EntityManager::get();
    for (auto mesh : g_meshes) {
        engine->destroy(mesh.vertexBuffer);
        engine->destroy(mesh.indexBuffer);
        engine->destroy(mesh.renderable);
        em.destroy(mesh.renderable);
    }
}

Texture* loadImage(Engine* engine, const char* name) {
    Path path(name);
    if (path.exists()) {
        std::ifstream inputStream(path, std::ios::binary);
        LinearImage* image = new image::LinearImage(ImageDecoder::decode(
            inputStream, path, ImageDecoder::ColorSpace::SRGB));
        if (!image->isValid()) {
            std::cerr << "The input image is invalid: " << path << std::endl;
            return nullptr;
        }

        inputStream.close();
        uint32_t channels = image->getChannels();
        uint32_t w = image->getWidth();
        uint32_t h = image->getHeight();
        std::cout << "channels: " << channels << " w:" << w << ", h:" << h << std::endl;
        Texture* texture = Texture::Builder()
                .width(w)
                .height(h)
                .levels(0xff)
                .format(channels == 3 ?
                        Texture::InternalFormat::RGB16F : Texture::InternalFormat::RGBA16F)
                .sampler(Texture::Sampler::SAMPLER_2D)
                .build(*engine);

        Texture::PixelBufferDescriptor::Callback freeCallback = [](void* buf, size_t, void* data) {
            delete reinterpret_cast<LinearImage*>(data);
        };

        Texture::PixelBufferDescriptor buffer(
                image->getPixelRef(),
                size_t(w * h * channels * sizeof(float)),
                channels == 3 ? Texture::Format::RGB : Texture::Format::RGBA,
                Texture::Type::FLOAT,
                freeCallback,
                image
        );

        texture->setImage(*engine, 0, std::move(buffer));
        texture->generateMipmaps(*engine);
        return texture;
    }
    return nullptr;
}

static Material *WindMaterial(Engine *engine) {
    MaterialBuilder builder;
    builder
        .name("WindMaterial")
        .targetApi(MaterialBuilder::TargetApi::OPENGL)
        .optimization(MaterialBuilderBase::Optimization::NONE)
        .require(VertexAttribute::UV0)
        .doubleSided(true)
        .depthCulling(false)
        .culling(MaterialBuilder::CullingMode::NONE)
        .blending(BlendingMode::FADE)
        .transparencyMode(TransparencyMode::TWO_PASSES_TWO_SIDES)
        // .parameter("BASECOLOR_MAP", MaterialBuilder::SamplerType::SAMPLER_2D)
        // .parameter("ALPHACOLOR_MAP", MaterialBuilder::SamplerType::SAMPLER_2D)
        .shading(Shading::UNLIT)
        .materialVertex(R"SHADER(
            void materialVertex(inout MaterialVertexInputs material) {
                vec4 vertexCoord = material.worldPosition;
                material.worldPosition = vertexCoord;
            }
        )SHADER")
        .material(R"SHADER(
            void material(inout MaterialInputs material) {
                prepareMaterial(material);
                // float4 alpha = texture(materialParams_ALPHACOLOR_MAP, getUV0());
                // float time = getUserTime().x;
                // float2 panOffset = vec2(time * 0.45, time * 0.06);
                // float2 uv = getUV0() - panOffset;
                // float2 uv = getUV0();
                // material.baseColor = texture(materialParams_BASECOLOR_MAP, uv);
                // material.baseColor.rgb *= material.baseColor.a;

                // material.emissive = texture(materialParams_BASECOLOR_MAP, uv);
                material.emissive = vec4(vec3(1.0, 1.0, 1.0), 0.0);
                // material.emissive *= texture(materialParams_BASECOLOR_MAP, uv);
                
            }
        )SHADER");
    Package pkg = builder.build(engine->getJobSystem());

    return Material::Builder().package(pkg.getData(), pkg.getSize())
            .build(*engine);
}


static float degree_to_radian(float degree) {
    return 180.0f / M_PI * degree;
}


static void setup(Engine* engine, View* view, Scene* scene) {
    // Texture* basecolor = loadImage(engine, "wind_layer_asset/Gradiant_Alpha_S.png");
    // Texture* quadTexture = loadImage(engine, "wind_assets/hvac_panel_front_bg.png");
    // if (!basecolor || !quadTexture) {
    //     std::cerr << "Texture image missing" << std::endl;
    //     return;
    // }

    MaterialBuilder::init();
    Material *windMaterial = WindMaterial(engine);
    const utils::CString windMaterialName("DefaultMaterial");
    g_materialInstances.registerMaterialInstance(windMaterialName, windMaterial->createInstance());

    TextureSampler sampler(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            TextureSampler::MagFilter::LINEAR, TextureSampler::WrapMode::REPEAT);

    // g_materialInstances.getMaterialInstance(windMaterialName)->setParameter(
    //     "BASECOLOR_MAP", basecolor, sampler);

    // g_materialInstances.getMaterialInstance(windMaterialName)->setParameter(
    //     "ALPHACOLOR_MAP", basecolor, sampler);

    auto& tcm = engine->getTransformManager();
    for (const auto& filename : g_filenames) {
        MeshReader::Mesh mesh  = MeshReader::loadMeshFromFile(engine, filename, g_materialInstances);
        if (mesh.renderable) {
            auto ei = tcm.getInstance(mesh.renderable);
            // tcm.setTransform(ei,
            //         mat4f::translation(float3{ 0, 0, 0}) *
            //         mat4f::rotation(degree_to_radian(60.0f), float3{ 0, 1, 0 })
            // ); 
            // tcm.setTransform(ei,
            //         mat4f::translation(float3{ 0, 0, 0}));
            tcm.setTransform(ei, mat4f{ mat3f(0.5), float3(0.0f, 0.0f, -2.0f) } *
            tcm.getWorldTransform(ei));
            // tcm.setTransform(ei, mat4f{mat3{0.5, 0.0, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 0.5},
            //                             float3(0.0f, 0.0f, 1.0f) } *
            //                      tcm.getWorldTransform(ei));
            scene->addEntity(mesh.renderable);
            g_meshes.push_back(mesh);
        }
    }
}




int main(int argc, char* argv[]) {
    utils::Path filename = "rect.filamesh";
    if (!filename.exists()) {
        std::cerr << "file not found!" << std::endl;
        return 1;
    }

    g_filenames.push_back(filename);
    
    g_config.title = "rect";
    
    FilamentApp& filamentApp = FilamentApp::get();
    filamentApp.run(g_config, setup, cleanup,
        FilamentApp::ImGuiCallback(),
        FilamentApp::PreRenderCallback(),
        FilamentApp::PostRenderCallback(), 2560, 980);

    return 0;
}


