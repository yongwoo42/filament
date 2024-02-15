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

const double HalfWidth = 10.52;
const double HalfHeight = 2.67;

Camera *cam;

static const Vertex QUAD_VERTICES[4] = {
    {{-HalfWidth, -HalfHeight}, {0, 0}},
    {{ HalfWidth, -HalfHeight}, {1, 0}},
    {{-HalfWidth,  HalfHeight}, {0, 1}},
    {{ HalfWidth,  HalfHeight}, {1, 1}},
};

static constexpr uint16_t QUAD_INDICES[6] = {
    0, 1, 2,
    3, 2, 1,
};

struct QuadInfo {
    Engine *engine;
    VertexBuffer* vb;
    IndexBuffer* ib;
    MaterialInstance* matInstance;
    Entity renderable;
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
        // .culling(MaterialBuilder::CullingMode::NONE)
        .blending(BlendingMode::FADE)
        .transparencyMode(TransparencyMode::TWO_PASSES_TWO_SIDES)
        .parameter("EMISSIVE_STRENGTH", MaterialBuilder::UniformType::FLOAT)
        .parameter("BASECOLOR_MAP", MaterialBuilder::SamplerType::SAMPLER_2D)
        .parameter("ROUGHNESS", MaterialBuilder::UniformType::FLOAT)
        .parameter("IOR", MaterialBuilder::UniformType::FLOAT)
        .parameter("ALPHACOLOR_MAP", MaterialBuilder::SamplerType::SAMPLER_2D)
        .shading(Shading::LIT)
        .materialVertex(R"SHADER(
            void materialVertex(inout MaterialVertexInputs material) {
                vec4 vertexCoord = material.worldPosition;
                float boundary = 50.0;
                vec2 origin1 = vertexCoord.xz + vec2(boundary, boundary);
                vec2 origin2 = vertexCoord.xz + vec2(-boundary, boundary);
                vec2 origin3 = vertexCoord.xz + vec2(boundary, -boundary);
                vec2 origin4 = vertexCoord.xz + vec2(-boundary, -boundary);

                float distance1 = length(origin1);
                float distance2 = length(origin2);
                float distance3 = length(origin3);
                float distance4 = length(origin4);
                float time = getUserTime().x * 2.0;

                float wave = sin(3.3 * PI * distance1 * 0.13 + time) * 0.125 +
                    sin(3.2 * PI * distance2 * 0.12 + time) * 0.125 +
                    sin(3.1 * PI * distance3 * 0.24 + time) * 0.125 +
                    sin(3.5 * PI * distance4 * 0.32 + time) * 0.125;
            
                
                // if (wave > 0.0) {
                //     vertexCoord.y += wave * material.uv0.x;
                // }
                
                // vertexCoord.x += wave * 0.1;
                material.worldPosition = vertexCoord;
                
            }
        )SHADER")
        .material(R"SHADER(
            void material(inout MaterialInputs material) {
                prepareMaterial(material);
                material.roughness = materialParams.ROUGHNESS;
                material.ior = materialParams.IOR;
                material.metallic = 0.0;

                float4 alpha = texture(materialParams_ALPHACOLOR_MAP, getUV0());
                float time = getUserTime().x;
                float2 panOffset = vec2(time * 0.45, time * 0.06);
                // float2 uv = getUV0() - panOffset;
                float2 uv = getUV0();
                
                material.baseColor = texture(materialParams_ALPHACOLOR_MAP, uv);
                material.baseColor.rgb *= material.baseColor.a;

                material.emissive = vec4(vec3(materialParams.EMISSIVE_STRENGTH), 0.0);
                material.emissive *= texture(materialParams_BASECOLOR_MAP, uv);
                material.baseColor.a = alpha.r * 0.299+ alpha.g * 0.587 + alpha.b * 0.114;
                if (getWorldPosition().z < 1.0) {
                    material.baseColor.a -= 0.5;
                }
            }
        )SHADER");
    Package pkg = builder.build(engine->getJobSystem());

    return Material::Builder().package(pkg.getData(), pkg.getSize())
            .build(*engine);
}

static Material *QuadMaterial(Engine *engine) {
    MaterialBuilder quadBuilder;
    quadBuilder.name("QuadMaterial")
        .targetApi(MaterialBuilder::TargetApi::OPENGL)
        .optimization(MaterialBuilderBase::Optimization::NONE)
        .require(VertexAttribute::UV0)
        .parameter("albedo", MaterialBuilder::SamplerType::SAMPLER_2D)
        .material(R"SHADER(
            void material(inout MaterialInputs material) {
                prepareMaterial(material);
                material.baseColor.rgb = texture(materialParams_albedo, getUV0()).rgb;
            }
        )SHADER")
        .shading(Shading::UNLIT);
    Package pkg = quadBuilder.build(engine->getJobSystem());
    return Material::Builder().package(pkg.getData(), pkg.getSize())
            .build(*engine);
}

static void createQuadRenderable(QuadInfo &quadInfo) {
    quadInfo.vb = VertexBuffer::Builder()
                .vertexCount(4)
                .bufferCount(1)
                .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT2, 0, 16)
                .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, 8, 16)
                .build(*quadInfo.engine);
    quadInfo.vb->setBufferAt(*quadInfo.engine, 0,
        VertexBuffer::BufferDescriptor(QUAD_VERTICES, 64, nullptr));
    quadInfo.ib = IndexBuffer::Builder()
                .indexCount(6)
                .bufferType(IndexBuffer::IndexType::USHORT)
                .build(*quadInfo.engine);    
    quadInfo.ib->setBuffer(*quadInfo.engine,
                IndexBuffer::BufferDescriptor(QUAD_INDICES, 12, nullptr));
    quadInfo.renderable = EntityManager::get().create();
    RenderableManager::Builder(1)
                .boundingBox({{ -HalfWidth, -HalfHeight, -5 }, { HalfWidth, HalfHeight, -5 }})
                .material(0, quadInfo.matInstance)
                .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, quadInfo.vb, quadInfo.ib, 0, 6)
                .culling(false)
                .receiveShadows(false)
                .castShadows(false)
                .build(*quadInfo.engine, quadInfo.renderable);    
}

static float degree_to_radian(float degree) {
    return 180.0f / M_PI * degree;
}


static void setup(Engine* engine, View* view, Scene* scene) {
    Texture* basecolor = loadImage(engine, "wind_assets/Windblow_fix.png");
    Texture* alphaChannel = loadImage(engine, "wind_assets/Windblow_fix-gradation.png");
    Texture* quadTexture = loadImage(engine, "wind_assets/hvac_panel_front_bg.png");
    if (!basecolor || !alphaChannel || !quadTexture) {
        std::cerr << "Texture image missing" << std::endl;
        return;
    }

    QuadInfo quadInfo;
    quadInfo.engine = engine;

    cam = engine->createCamera(utils::EntityManager::get().create());
    view->setCamera(cam);
    cam->setProjection(Camera::Projection::ORTHO,
        -HalfWidth, HalfWidth, -HalfHeight, HalfHeight,
        -5.0, 0.0);

    MaterialBuilder::init();
    Material *windMaterial = WindMaterial(engine);
    const utils::CString windMaterialName("DefaultMaterial");
    g_materialInstances.registerMaterialInstance(windMaterialName, windMaterial->createInstance());
    Material *quadMaterial = QuadMaterial(engine);
    const utils::CString quadMaterialName("QuadMaterial");
    g_materialInstances.registerMaterialInstance(quadMaterialName, quadMaterial->createInstance());

    TextureSampler sampler(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            TextureSampler::MagFilter::LINEAR, TextureSampler::WrapMode::REPEAT);

    g_materialInstances.getMaterialInstance(windMaterialName)->setParameter("BASECOLOR_MAP", basecolor, sampler);
    g_materialInstances.getMaterialInstance(windMaterialName)->setParameter("ROUGHNESS", 0.482f);
    g_materialInstances.getMaterialInstance(windMaterialName)->setParameter("IOR", 1.5f);
    g_materialInstances.getMaterialInstance(windMaterialName)->setParameter("ALPHACOLOR_MAP", alphaChannel, sampler);
    g_materialInstances.getMaterialInstance(windMaterialName)->setParameter("EMISSIVE_STRENGTH", 2.0f);

    g_materialInstances.getMaterialInstance(quadMaterialName)->setParameter("albedo", quadTexture, sampler);
    quadInfo.matInstance = g_materialInstances.getMaterialInstance(quadMaterialName);
    // createQuadRenderable(quadInfo);
    // scene->addEntity(quadInfo.renderable);

    auto& tcm = engine->getTransformManager();
    for (const auto& filename : g_filenames) {
        MeshReader::Mesh mesh  = MeshReader::loadMeshFromFile(engine, filename, g_materialInstances);
        if (mesh.renderable) {
            auto ei = tcm.getInstance(mesh.renderable);
            
            // tcm.setTransform(ei,
            //         mat4f::translation(float3{ 0, 0, 0}) *
            //         mat4f::rotation(degree_to_radian(60.0f), float3{ 0, 1, 0 })
            // ); 
            tcm.setTransform(ei, mat4f{mat3{15.0, 0.0, 0.0, 0.0, 18.0, 0.0, 0.0, 0.0, 14.0},
                                        float3(0.0f, 0.0f, 1.0f) } *
                                 tcm.getWorldTransform(ei));
            scene->addEntity(mesh.renderable);
            g_meshes.push_back(mesh);
        }
    }
}




int main(int argc, char* argv[]) {
    utils::Path filename = "wind_assets/wind.filamesh";
    if (!filename.exists()) {
        std::cerr << "file not found!" << std::endl;
        return 1;
    }
    g_filenames.push_back(filename);
    
    g_config.title = "air vent";
    
    FilamentApp& filamentApp = FilamentApp::get();

    filamentApp.run(g_config, setup, cleanup,
        FilamentApp::ImGuiCallback(),
        FilamentApp::PreRenderCallback(),
        FilamentApp::PostRenderCallback(), 2560, 980);

    return 0;
}


