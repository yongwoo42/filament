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
        .parameter("MASK_TEXTURE", MaterialBuilder::SamplerType::SAMPLER_2D)
        .shading(Shading::UNLIT)
        .materialVertex(R"SHADER(
            void materialVertex(inout MaterialVertexInputs material) {
                vec4 vertexCoord = material.worldPosition;
                float boundary = 1.0;
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
            
                vertexCoord.y += wave * 0.5 * (1.0 - material.uv0.y);
                material.worldPosition = vertexCoord;
            }
        )SHADER")
        .material(R"SHADER(
            float noise_randomValue (float2 uv) {
                return fract(sin(dot(uv, float2(12.9898, 78.233)))*43758.5453);
            }

            float noise_interpolate (float a, float b, float t) {
                return (1.0-t)*a + (t*b);
            }

            float valueNoise (float2 uv) {
                float2 i = floor(uv);
                float2 f = fract(uv);
                f = f * f * (3.0 - 2.0 * f);

                uv = abs(fract(uv) - 0.5);
                float2 c0 = i + float2(0.0, 0.0);
                float2 c1 = i + float2(1.0, 0.0);
                float2 c2 = i + float2(0.0, 1.0);
                float2 c3 = i + float2(1.0, 1.0);
                float r0 = noise_randomValue(c0);
                float r1 = noise_randomValue(c1);
                float r2 = noise_randomValue(c2);
                float r3 = noise_randomValue(c3);

                float bottomOfGrid = noise_interpolate(r0, r1, f.x);
                float topOfGrid = noise_interpolate(r2, r3, f.x);
                float t = noise_interpolate(bottomOfGrid, topOfGrid, f.y);
                return t;
            }


            void SimpleNoise_float(float2 UV, float Scale, out float Out)
            {
                float t = 0.0;

                float freq = pow(2.0, float(0));
                float amp = pow(0.5, float(3-0));
                t += valueNoise(float2(UV.x*Scale/freq, UV.y*Scale/freq))*amp;

                freq = pow(2.0, float(1));
                amp = pow(0.5, float(3-1));
                t += valueNoise(float2(UV.x*Scale/freq, UV.y*Scale/freq))*amp;

                freq = pow(2.0, float(2));
                amp = pow(0.5, float(3-2));
                t += valueNoise(float2(UV.x*Scale/freq, UV.y*Scale/freq))*amp;

                Out = t;
            }

            void tilingAndOffset(float2 UV, float2 Tiling, float2 Offset, out float2 Out) {
                    Out = UV * Tiling + Offset;
            }

            float3 f_f3(float f) {
                return fract(f / float3(16777216, 65536, 256)); 
            }

            void power(float a, float b, out float Out) {
                Out = pow(a, b);
            }


            void material(inout MaterialInputs material) {
                prepareMaterial(material);
                float2 uv = getUV0();
                float2 speed = float2(0.0, 0.5);
                float time = getUserTime().x;
                float nodeOut = 0.0;
                float4 maskOut = float4(0.0);

                float2 offset = speed * time;

                float4 mask = texture(materialParams_MASK_TEXTURE, uv);


                float2 afterTilingAndOffset = float2(0.0, 0.0);
                tilingAndOffset(uv,
                    float2(3.0, 0.2),
                    offset,
                    afterTilingAndOffset);

                SimpleNoise_float(afterTilingAndOffset, 10.0, nodeOut);

                power(nodeOut, 1.7, nodeOut);

                maskOut = mask * nodeOut;

                float4 color = maskOut;
                material.baseColor.rgb = vec3(color[0], color[1], color[2]);
                
                material.baseColor.a = 0.0;
                //material.baseColor.a = color[3];    
                

                // material.emissive = texture(materialParams_BASECOLOR_MAP, uv);
                // material.emissive = vec4(vec3(0.0), 0.0);
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
    Texture* maskSampler = loadImage(engine, "mask.png");
    if (!maskSampler) {
        std::cerr << "Texture image missing" << std::endl;
        return;
    }

    // scene->setSkybox(Skybox::Builder().color({0.0, 0.78, 0.0, 1.0}).build(*engine));

    MaterialBuilder::init();
    Material *windMaterial = WindMaterial(engine);
    const utils::CString windMaterialName("DefaultMaterial");
    g_materialInstances.registerMaterialInstance(windMaterialName, windMaterial->createInstance());

    TextureSampler sampler(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            TextureSampler::MagFilter::LINEAR, TextureSampler::WrapMode::REPEAT);

    g_materialInstances.getMaterialInstance(windMaterialName)->setParameter(
        "MASK_TEXTURE", maskSampler, sampler);

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
            tcm.setTransform(ei, mat4f{ mat3f(10.0), float3(0.0f, 0.0f, -10.0f) } *
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
    utils::Path filename = "wind_t2.filamesh";
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


