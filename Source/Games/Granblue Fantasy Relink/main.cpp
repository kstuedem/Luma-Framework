// Granblue Fantasy Relink - DLAA/FSRAA Implementation
// Replaces TAA with DLSS/FSR in DLAA (no upscaling) mode
// Based on the FFXV Luma implementation

#include <d3d11.h>
#define GAME_GRANBLUE_FANTASY_RELINK 1

#define ENABLE_NGX 1
#define ENABLE_FIDELITY_SK 1

#include "..\..\Core\core.hpp"
#include "includes\cbuffers.h"
#include <cstring>

namespace
{
   ShaderHashesList shader_hashes_TAA;
   ShaderHashesList shader_hashes_scene_buffer_dispatch; // 0xDA85F5BB - the CS pass with SceneBuffer (cb0)
   const uint32_t Global_buffer_size = 0x3000000;
   const uint32_t CBSceneBuffer_size = sizeof(cbSceneBuffer);
   // const uint32_t Global_buffer_capture_unmap_index = 10; // 1 = first unmap, 2 = second unmap, etc.

} // namespace

struct GameDeviceDataGBFR final : public GameDeviceData
{
#if ENABLE_SR
   // SR - Resources extracted from TAA pass
   com_ptr<ID3D11Resource> sr_source_color;    // t3: current color
   com_ptr<ID3D11Resource> depth_buffer;        // t5: depth
   com_ptr<ID3D11Resource> sr_motion_vectors;   // t23: motion vectors (already decoded)
   com_ptr<ID3D11Resource> taa_rt0_resource; // seems unused
   com_ptr<ID3D11Resource> taa_rt1_resource;
   D3D11_TEXTURE2D_DESC taa_rt1_desc;
   std::atomic<ID3D11DeviceContext*> draw_device_context = nullptr;
   ID3D11CommandList* remainder_command_list = nullptr; // Raw pointer for identity comparison only (no AddRef to avoid desync with reshade's proxy ref tracking)
   com_ptr<ID3D11CommandList> partial_command_list;
   com_ptr<ID3D11Buffer> modifiable_index_vertex_buffer;
   std::atomic<bool> output_supports_uav = false;
   std::atomic<bool> output_changed = false;

#endif // ENABLE_SR

   ID3D11Buffer* global_buffer = nullptr;
   void* global_buffer_map_data = nullptr;
   uint64_t global_buffer_map_size = 0;
   std::unique_ptr<uint8_t[]> global_buffer_copy;
   bool has_global_buffer_copy = false;
   // uint32_t global_buffer_unmap_count_this_frame = 0;

   // Camera data extracted from the projection matrix
   std::atomic<bool> has_camera_data = false;
   float camera_fov = 60.0f * (3.14159265f / 180.0f);
   float camera_near = 0.1f;
   float camera_far = 1000.0f;
   float2 jitter = {0, 0};
};

class GranblueFantasyRelink final : public Game
{
   static GameDeviceDataGBFR& GetGameDeviceData(DeviceData& device_data)
   {
      return *static_cast<GameDeviceDataGBFR*>(device_data.game);
   }

#include "includes\dlss_helpers.hpp"

public:
   void OnInit(bool async) override
   {
      luma_settings_cbuffer_index = 9;
      luma_data_cbuffer_index = 8;
   }

   void OnLoad(std::filesystem::path& file_path, bool failed) override
   {
      if (!failed)
      {
         reshade::register_event<reshade::addon_event::execute_secondary_command_list>(GranblueFantasyRelink::OnExecuteSecondaryCommandList);
         reshade::register_event<reshade::addon_event::map_buffer_region>(GranblueFantasyRelink::OnMapBufferRegion);
         reshade::register_event<reshade::addon_event::unmap_buffer_region>(GranblueFantasyRelink::OnUnmapBufferRegion);
      }
   }

   DrawOrDispatchOverrideType OnDrawOrDispatch(
      ID3D11Device* native_device,
      ID3D11DeviceContext* native_device_context,
      CommandListData& cmd_list_data,
      DeviceData& device_data,
      reshade::api::shader_stage stages,
      const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes,
      bool is_custom_pass,
      bool& updated_cbuffers,
      std::function<void()>* original_draw_dispatch_func) override
   {
      auto& game_device_data = GetGameDeviceData(device_data);

      // =========================================================================
      // Detect the 0xDA85F5BB compute shader pass to get SceneBuffer offset
      // and parse the latest copied cbuffer snapshot at that offset.
      // =========================================================================
      if (original_shader_hashes.Contains(shader_hashes_scene_buffer_dispatch))
      {
         com_ptr<ID3D11DeviceContext1> context1;
         native_device_context->QueryInterface(IID_PPV_ARGS(&context1));

         if (context1)
         {
            com_ptr<ID3D11Buffer> cs_cb0;
            UINT first_constant = 0;
            UINT num_constants = 0;
            context1->CSGetConstantBuffers1(0, 1, &cs_cb0, &first_constant, &num_constants);

            if (cs_cb0.get())
            {
               if (game_device_data.has_global_buffer_copy)
               {
                  const uint8_t* cb_data = game_device_data.global_buffer_copy.get() + (first_constant * 16); // Convert from constants to bytes

                  ExtractCameraData(game_device_data, cb_data);
                  game_device_data.has_camera_data = true;
                  
               }
            }
         }

         return DrawOrDispatchOverrideType::None;
      }

      // =========================================================================
      // TAA Pass Handling - Replace TAA with SR (DLAA/FSRAA mode, no upscaling)
      // =========================================================================
      if (device_data.sr_type != SR::Type::None &&
          !device_data.sr_suppressed &&
          original_shader_hashes.Contains(shader_hashes_TAA))
      {

         device_data.taa_detected = true;

         // We need camera data to run SR. On the first frame we won't have it yet
         // since we extract it one frame late (on unmap after caching the buffer).
         if (!game_device_data.has_global_buffer_copy)
         {
            device_data.force_reset_sr = true;
            return DrawOrDispatchOverrideType::None;
         }

         // Extract TAA shader resources (source color, depth, motion vectors)
         if (!ExtractTAAShaderResources(native_device, native_device_context, game_device_data))
         {
            ASSERT_ONCE(false);
            return DrawOrDispatchOverrideType::None;
         }

         device_data.has_drawn_sr = true;

         // Get render targets (TAA writes to RT0 and RT1)
         com_ptr<ID3D11RenderTargetView> render_target_views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
         com_ptr<ID3D11DepthStencilView> depth_stencil_view;
         native_device_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, &render_target_views[0], &depth_stencil_view);
         if (render_target_views[1].get() == nullptr)
         {
            return DrawOrDispatchOverrideType::None;
         }

         // Setup output texture

         render_target_views[1]->GetResource(&game_device_data.taa_rt1_resource);

         if (!SetupSROutput(native_device, device_data, game_device_data))
         {
            return DrawOrDispatchOverrideType::None;
         }


         native_device_context->FinishCommandList(TRUE, &game_device_data.partial_command_list);
         if (game_device_data.modifiable_index_vertex_buffer)
         {
            D3D11_MAPPED_SUBRESOURCE mapped_buffer;
            // When starting a new command list first map has to be D3D11_MAP_WRITE_DISCARD
            native_device_context->Map(game_device_data.modifiable_index_vertex_buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_buffer);
            native_device_context->Unmap(game_device_data.modifiable_index_vertex_buffer.get(), 0);
         }
         game_device_data.draw_device_context = native_device_context;
         if (device_data.has_drawn_sr)
         {
            return DrawOrDispatchOverrideType::Replaced;
         }
      }

      return DrawOrDispatchOverrideType::None;
   }

   // =========================================================================
   // Buffer map/unmap callbacks to snapshot SceneBuffer writes into memory.
   // No parsing happens here; dispatch parses using firstConstant.
   // =========================================================================
   static void OnMapBufferRegion(
      reshade::api::device* device,
      reshade::api::resource resource,
      uint64_t offset,
      uint64_t size,
      reshade::api::map_access access,
      void** data)
   {
      DeviceData& device_data = *device->get_private_data<DeviceData>();
      auto& game_device_data = GetGameDeviceData(device_data);

      // Only intercept write accesses
      if (access != reshade::api::map_access::write_only &&
          access != reshade::api::map_access::write_discard &&
          access != reshade::api::map_access::read_write)
         return;

      ID3D11Buffer* buffer = reinterpret_cast<ID3D11Buffer*>(resource.handle);
      D3D11_BUFFER_DESC buffer_desc;
      buffer->GetDesc(&buffer_desc);
      if (buffer_desc.BindFlags == (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER))
      {
         auto& device_data = *device->get_private_data<DeviceData>();
         auto& game_device_data = GetGameDeviceData(device_data);

         game_device_data.modifiable_index_vertex_buffer = (ID3D11Buffer*)resource.handle;
      }
      
      if (size != UINT64_MAX || buffer_desc.ByteWidth != Global_buffer_size)
         return;

      // Save mapped info so unmap can copy bytes into our CPU snapshot.
      game_device_data.global_buffer = buffer;
      game_device_data.global_buffer_map_data = *data;
      game_device_data.global_buffer_map_size = buffer_desc.ByteWidth;
   }

   static void OnUnmapBufferRegion(
      reshade::api::device * device,
      reshade::api::resource resource)
   {
      DeviceData& device_data = *device->get_private_data<DeviceData>();
      auto& game_device_data = GetGameDeviceData(device_data);

      ID3D11Buffer* buffer = reinterpret_cast<ID3D11Buffer*>(resource.handle);

      if (game_device_data.global_buffer != buffer)
         return;

      // Check all conditions: right buffer and we have mapped bytes to copy
      if (game_device_data.global_buffer_map_data != nullptr &&
          game_device_data.global_buffer_map_size > 0 &&
          !game_device_data.has_global_buffer_copy)
      {
         if (game_device_data.global_buffer_copy.get() == nullptr)
         {
            game_device_data.global_buffer_copy = std::make_unique<uint8_t[]>(static_cast<size_t>(game_device_data.global_buffer_map_size));
         }
         std::memcpy(
            game_device_data.global_buffer_copy.get(),
            game_device_data.global_buffer_map_data,
            static_cast<size_t>(game_device_data.global_buffer_map_size));
         game_device_data.has_global_buffer_copy = true;

         game_device_data.global_buffer = nullptr;
         game_device_data.global_buffer_map_data = nullptr;
         game_device_data.global_buffer_map_size = 0;
      }
   }

   static void OnExecuteSecondaryCommandList(reshade::api::command_list* cmd_list, reshade::api::command_list* secondary_cmd_list)
   {
      com_ptr<ID3D11DeviceContext> native_device_context;
      ID3D11DeviceChild* device_child = (ID3D11DeviceChild*)(cmd_list->get_native());
      HRESULT hr = device_child->QueryInterface(&native_device_context);

      auto& device_data = *cmd_list->get_device()->get_private_data<DeviceData>();
      auto& game_device_data = GetGameDeviceData(device_data);

      if (native_device_context)
      {
         com_ptr<ID3D11CommandList> native_command_list;
         ID3D11DeviceChild* device_child = (ID3D11DeviceChild*)(secondary_cmd_list->get_native());
         HRESULT hr = device_child->QueryInterface(&native_command_list);
         if (native_command_list.get() == game_device_data.remainder_command_list && game_device_data.partial_command_list.get() != nullptr)
         {
            native_device_context->ExecuteCommandList(game_device_data.partial_command_list.get(), FALSE);
            game_device_data.partial_command_list.reset();

            CommandListData& cmd_list_data = *cmd_list->get_private_data<CommandListData>();
                     // Get SR instance data
            auto* sr_instance_data = device_data.GetSRInstanceData();
            // DLAA mode: render resolution == output resolution (no upscaling)
            {
               SR::SettingsData settings_data;
               settings_data.output_width = game_device_data.taa_rt1_desc.Width;
               settings_data.output_height = game_device_data.taa_rt1_desc.Height;
               settings_data.render_width = game_device_data.taa_rt1_desc.Width;
               settings_data.render_height = game_device_data.taa_rt1_desc.Height;
               settings_data.dynamic_resolution = false;
               settings_data.hdr = true;
               settings_data.auto_exposure = true; // No exposure texture extraction for now
               settings_data.inverted_depth = false;
               settings_data.mvs_jittered = false; // Granblue MVs are already unjittered
               settings_data.render_preset = dlss_render_preset;
               sr_implementations[device_data.sr_type]->UpdateSettings(sr_instance_data, native_device_context.get(), settings_data);
            }
            // Prepare SR draw data
            {
               bool reset_sr = device_data.force_reset_sr || game_device_data.output_changed;
               device_data.force_reset_sr = false;

               SR::SuperResolutionImpl::DrawData draw_data;
               draw_data.source_color = game_device_data.sr_source_color.get();
               draw_data.output_color = device_data.sr_output_color.get();
               draw_data.motion_vectors = game_device_data.sr_motion_vectors.get();
               draw_data.depth_buffer = game_device_data.depth_buffer.get();
               draw_data.pre_exposure = 0.0f;
               draw_data.jitter_x = 0;
               draw_data.jitter_y = 0;
               draw_data.vert_fov = game_device_data.camera_fov;
               draw_data.far_plane = game_device_data.camera_far;
               draw_data.near_plane = game_device_data.camera_near;
               draw_data.reset = reset_sr;
               draw_data.render_width = game_device_data.taa_rt1_desc.Width;
               draw_data.render_height = game_device_data.taa_rt1_desc.Height;

               // Cache and restore state around SR execution
               DrawStateStack<DrawStateStackType::FullGraphics> draw_state_stack;
               DrawStateStack<DrawStateStackType::Compute> compute_state_stack;
               draw_state_stack.Cache(native_device_context.get(), device_data.uav_max_count);
               compute_state_stack.Cache(native_device_context.get(), device_data.uav_max_count);

               // Execute SR
               device_data.has_drawn_sr = sr_implementations[device_data.sr_type]->Draw(sr_instance_data, native_device_context.get(), draw_data);
#if DEVELOPMENT
            // Add trace info for DLSS/FSR execution
               if (device_data.has_drawn_sr)
               {
                  const std::shared_lock lock_trace(s_mutex_trace);
                  if (trace_running)
                  {
                     const std::unique_lock lock_trace_2(cmd_list_data.mutex_trace);
                     TraceDrawCallData trace_draw_call_data;
                     trace_draw_call_data.type = TraceDrawCallData::TraceDrawCallType::Custom;
                     trace_draw_call_data.command_list = native_device_context;
                     trace_draw_call_data.custom_name = device_data.sr_type == SR::Type::DLSS ? "DLAA" : "FSRAA";
                     GetResourceInfo(device_data.sr_output_color.get(), trace_draw_call_data.rt_size[0], trace_draw_call_data.rt_format[0], &trace_draw_call_data.rt_type_name[0], &trace_draw_call_data.rt_hash[0]);
                     cmd_list_data.trace_draw_calls_data.insert(cmd_list_data.trace_draw_calls_data.end() - 1, trace_draw_call_data);
                  }
               }
#endif
               draw_state_stack.Restore(native_device_context.get());
               compute_state_stack.Restore(native_device_context.get());

            }

            // Clear temporary resources
            game_device_data.sr_source_color = nullptr;
            game_device_data.depth_buffer = nullptr;
            game_device_data.sr_motion_vectors = nullptr;

            // Handle SR result
            if (device_data.has_drawn_sr)
            {
               if (!game_device_data.output_supports_uav)
               {
                  native_device_context->CopyResource(game_device_data.taa_rt1_resource.get(), device_data.sr_output_color.get());
               }
            }
            else
            {
               device_data.force_reset_sr = true;
            }
            game_device_data.taa_rt1_resource = nullptr;
            if (!game_device_data.output_supports_uav)
            {
               device_data.sr_output_color = nullptr;
            }
         }  
      }

      com_ptr<ID3D11CommandList> native_command_list;
      hr = device_child->QueryInterface(&native_command_list);
      if (native_command_list)
      {
         ID3D11DeviceChild* device_child = (ID3D11DeviceChild*)(secondary_cmd_list->get_native());
         hr = device_child->QueryInterface(&native_device_context);
         if (native_device_context == game_device_data.draw_device_context)
         {
            game_device_data.remainder_command_list = native_command_list.get();
         }
      }
   }

   void OnCreateDevice(ID3D11Device* native_device, DeviceData& device_data) override
   {
      device_data.game = new GameDeviceDataGBFR;
   }

   void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
   {
      auto& game_device_data = GetGameDeviceData(device_data);

      if (!device_data.has_drawn_sr)
      {
         device_data.force_reset_sr = true;
      }
      device_data.has_drawn_sr = false;
      game_device_data.has_global_buffer_copy = false;
      game_device_data.remainder_command_list = nullptr;
      game_device_data.draw_device_context = nullptr;
   }

   void PrintImGuiAbout() override
   {
      ImGui::Text("Granblue Fantasy Relink Luma mod - DLAA/FSRAA", "");
   }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   if (ul_reason_for_call == DLL_PROCESS_ATTACH)
   {
      Globals::SetGlobals(PROJECT_NAME, "Granblue Fantasy Relink");
      Globals::DEVELOPMENT_STATE = Globals::ModDevelopmentState::Playable;
      Globals::VERSION = 1;

      // TAA pixel shader hash
      shader_hashes_TAA.pixel_shaders.emplace(std::stoul("478E345C", nullptr, 16));

      // 0xDA85F5BB compute shader that has SceneBuffer with projection matrix and jitter
      shader_hashes_scene_buffer_dispatch.compute_shaders.emplace(std::stoul("DA85F5BB", nullptr, 16));
      force_disable_display_composition = true;
      swapchain_format_upgrade_type = TextureFormatUpgradesType::None;
      swapchain_upgrade_type = SwapchainUpgradeType::None;
      texture_format_upgrades_type = TextureFormatUpgradesType::None;

      texture_upgrade_formats = {
         reshade::api::format::r8g8b8a8_unorm,
         reshade::api::format::r8g8b8a8_unorm_srgb,
         reshade::api::format::r8g8b8a8_typeless,
         reshade::api::format::r8g8b8x8_unorm,
         reshade::api::format::r8g8b8x8_unorm_srgb,
         reshade::api::format::b8g8r8a8_unorm,
         reshade::api::format::b8g8r8a8_unorm_srgb,
         reshade::api::format::b8g8r8a8_typeless,
         reshade::api::format::b8g8r8x8_unorm,
         reshade::api::format::b8g8r8x8_unorm_srgb,
         reshade::api::format::b8g8r8x8_typeless,
         reshade::api::format::r11g11b10_float,
      };

      texture_format_upgrades_2d_size_filters = 0 | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainResolution | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainAspectRatio;

#if DEVELOPMENT
      forced_shader_names.emplace(std::stoul("478E345C", nullptr, 16), "TAA");
      forced_shader_names.emplace(std::stoul("DA85F5BB", nullptr, 16), "SceneBuffer_CS");
#endif

      game = new GranblueFantasyRelink();
   }
   else if (ul_reason_for_call == DLL_PROCESS_DETACH)
   {
      reshade::unregister_event<reshade::addon_event::execute_secondary_command_list>(GranblueFantasyRelink::OnExecuteSecondaryCommandList);
      reshade::unregister_event<reshade::addon_event::map_buffer_region>(GranblueFantasyRelink::OnMapBufferRegion);
      reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(GranblueFantasyRelink::OnUnmapBufferRegion);
   }

   CoreMain(hModule, ul_reason_for_call, lpReserved);

   return TRUE;
}
