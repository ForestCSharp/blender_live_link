#pragma once

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

#include "live_link/live_link_decode.h"
#include "core/dynamic_array.h"
#include "render/passes/imgui/imgui_layer.h"
#include "state/state.h"

namespace LiveLinkSystem
{
	// Writes the payload to GAME2_LIVE_LINK_CAPTURE when it is the largest seen
	// so far. The full-scene sync is by far the biggest message, so this lands a
	// self-contained scene that `--no-live-link -f <path>` can replay for
	// benchmarking without driving Blender.
	void capture_flatbuffer_payload(const DynamicArray<u8>& in_payload)
	{
		const std::optional<std::string>& path = RuntimeConfig::get().live_link_capture;
		if (!path)
		{
			return;
		}

		static size_t largest_captured_bytes = 0;
		if (in_payload.length() <= largest_captured_bytes)
		{
			return;
		}

		FILE* file = fopen(path->c_str(), "wb");
		if (!file)
		{
			printf("[live-link] failed to open capture file: %s\n", path->c_str());
			return;
		}

		const size_t written = fwrite(in_payload.data(), 1, in_payload.length(), file);
		fclose(file);
		if (written != in_payload.length())
		{
			printf("[live-link] short write to capture file: %s\n", path->c_str());
			return;
		}

		largest_captured_bytes = in_payload.length();
		printf("[live-link] captured %zu bytes to %s\n", written, path->c_str());
	}

	void parse_flatbuffer_data(const DynamicArray<u8>& payload)
	{
		auto update = decode_update({payload.data(), payload.length()});
		if (update)
		{
			capture_flatbuffer_payload(payload);
			state.live_link.scene_updates.send(std::move(*update));
		}
	}

	// Live Link Function. Runs on its own thread
	void live_link_thread_function()
	{
		socket_lib_init();
	
		// Init socket we'll use to talk to blender
		struct addrinfo hints, *res;
	
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
	
		const char* HOST = "127.0.0.1";
	
		// getaddrinfo can transiently fail (system resolver hiccups) — retry
		// instead of dereferencing a null result
		res = nullptr;
		while (state.runtime.game_running)
		{
			const int getaddrinfo_result = getaddrinfo(HOST, state.live_link.port.c_str(), &hints, &res);
			if (getaddrinfo_result == 0 && res != nullptr)
			{
				break;
			}
	
			printf("live link: getaddrinfo failed (%s), retrying\n", gai_strerror(getaddrinfo_result));
			res = nullptr;
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		if (res == nullptr)
		{
			return;	// shutting down before the resolver ever succeeded
		}
	
		// make a socket
		state.live_link.blender_socket = socket_open(res->ai_family, res->ai_socktype, res->ai_protocol);

		// Reuse the address so a restart isn't blocked by our own TIME_WAIT
		// sockets. Deliberately NOT SO_REUSEPORT: on macOS/BSD that lets a
		// second instance bind the same port and silently steal Blender's
		// connection instead of reporting the conflict.
		socket_set_reuse_addr(state.live_link.blender_socket, true);

		// Bind + listen. A live link port that's already taken is a normal
		// misconfiguration (another game instance, or game_web/bridge.py), not
		// a fatal error: report it and run without live link rather than
		// exiting from this thread while the render loop is still going.
		const bool bind_failed = bind(
			state.live_link.blender_socket, res->ai_addr, res->ai_addrlen) != 0;
		const int backlog = 1;
		const bool listen_failed =
			!bind_failed && listen(state.live_link.blender_socket, backlog) != 0;

		freeaddrinfo(res);

		if (bind_failed || listen_failed)
		{
			const int error = socket_get_last_error();
			printf("live link: failed to %s 127.0.0.1:%s (error %i%s)\n",
				bind_failed ? "bind" : "listen", state.live_link.port.c_str(), error,
				error == socket_error_addr_in_use() ? ": port already in use" : "");
			if (error == socket_error_addr_in_use())
			{
				printf("live link: another process is holding the port. Find it with\n"
					"  lsof -nP -iTCP:%s -sTCP:LISTEN\n"
					"and stop it (a stale game instance, or game_web/bridge.py from "
					"./build.sh -web), or pass --port to use a different one.\n",
					state.live_link.port.c_str());
			}
			printf("live link: continuing without live link; Blender updates will not arrive.\n");
			socket_close(state.live_link.blender_socket);
			state.live_link.blender_socket = socket_invalid();
			socket_lib_quit();
			return;
		}

		// Read exactly one prefix/body at a time: TCP may split a prefix or
		// coalesce several updates. Timeouts retain the partial frame.
		auto receive_exact = [&](u8* destination, size_t count) -> bool
		{
			size_t offset = 0;
			while (state.runtime.game_running && offset < count)
			{
				const int received = (int) socket_recv(
					state.live_link.connection_socket, destination + offset,
					std::min(count - offset, size_t(1) << 20), 0); // capped so the int return can't overflow
				if (received == 0)
					return false; // EOF: discard this connection's partial frame.
				if (received < 0)
				{
					const int error = socket_get_last_error();
					if (error == socket_error_again() || error == socket_error_would_block()
						|| error == socket_error_timed_out())
					{
						socket_wait_readable(state.live_link.connection_socket, 250);
						continue;
					}
					printf("live link: receive error %i; waiting for reconnect\n", error);
					return false;
				}
				offset += (size_t) received;
			}
			return offset == count;
		};

		while (state.runtime.game_running)
		{
			// Bounded wait so shutdown (game_running = false) is seen within 250ms.
			if (!socket_wait_readable(state.live_link.blender_socket, 250))
				continue;

			struct sockaddr_storage their_addr;
			socklen_t addr_size = sizeof their_addr;
			state.live_link.connection_socket = accept(
				state.live_link.blender_socket, (struct sockaddr*) &their_addr, &addr_size);
			if (!socket_is_valid(state.live_link.connection_socket))
				continue;

			while (state.runtime.game_running)
			{
				u8 prefix[sizeof(flatbuffers::uoffset_t)];
				if (!receive_exact(prefix, sizeof(prefix)))
					break;
				const auto body_size = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(prefix);
				// DynamicArray uses signed 32-bit lengths.
				if (body_size == 0 || body_size > 0x7fffffffu - sizeof(prefix))
					break;
				DynamicArray<u8> flatbuffer_data;
				flatbuffer_data.add_uninitialized((i32)(sizeof(prefix) + body_size));
				memcpy(flatbuffer_data.data(), prefix, sizeof(prefix));
				if (!receive_exact(flatbuffer_data.data() + sizeof(prefix), body_size))
					break;
				parse_flatbuffer_data(flatbuffer_data);
			}
			socket_close(state.live_link.connection_socket);
			state.live_link.connection_socket = INVALID_SOCKET;
		}

		printf("Shutting down sockets\n");
	
		socket_close(state.live_link.connection_socket);
		socket_close(state.live_link.blender_socket);
	
		socket_lib_quit();
	}
	
	// Retirement keeps old views/allocations alive until their frame fence.
	void retire_image(GpuImage& in_image)
	{
		ImGuiLayer::unregister_texture(in_image.view);
		for (VkImageView layer_view : in_image.layer_views)
		{
			ImGuiLayer::unregister_texture(layer_view);
		}
		vulkan_context_retire_image(&state.vk, in_image);
	}

	GpuImage create_image(const PendingImage& in_pending)
	{
		// UNORM (not SRGB): the addon sends linear-encoded bytes (Blender
		// image.pixels are scene-linear floats * 255). Banding in
		// darks is the known tradeoff until the addon sends sRGB-encoded data.
		return gpu_image_create_from_data(
			&state.vk,
			(u32) in_pending.width,
			(u32) in_pending.height,
			VK_FORMAT_R8G8B8A8_UNORM,
			in_pending.pixels.data(),
			in_pending.pixels.size()
		);
	}

	void register_image(PendingImage in_pending)
	{
		if (auto found = state.images.id_to_index.find(in_pending.unique_id); found != state.images.id_to_index.end())
		{
			// Same slot, new image: the per-frame descriptor caches see the new
			// view and rewrite their bindless entry.
			const i32 index = found->second;
			if (state.images.content_hashes[index] != in_pending.content_hash)
			{
				retire_image(state.images.items[index]);
				state.images.items[index] = create_image(in_pending);
				state.images.content_hashes[index] = in_pending.content_hash;
			}
			return;
		}

		if (state.images.items.length() >= MAX_BINDLESS_IMAGES)
		{
			printf("Exceeded MAX_BINDLESS_IMAGES (%i); skipping image UID %i\n", MAX_BINDLESS_IMAGES, in_pending.unique_id);
			return;
		}
	
		state.images.id_to_index[in_pending.unique_id] = (i32) state.images.items.length();
		state.images.items.add(create_image(in_pending));
		state.images.content_hashes.add(in_pending.content_hash);
	}

	// Images are only destroyed on scene reset or replaced in place;
	// individual removal is unsupported.
	void reset_images()
	{
		for (GpuImage& image : state.images.items)
		{
			retire_image(image);
		}
		state.images.items.reset();
		state.images.content_hashes.reset();
		state.images.id_to_index.clear();
	}
	
	// Registers one material (main thread). Returns true when the GPU buffer
	// needs an upload: newly appended, or an existing id whose values changed
	// (Blender resends every referenced material with each export).
	bool register_material(const PendingMaterial& in_pending)
	{
		auto existing = state.materials.id_to_index.find(in_pending.unique_id);
		const bool is_new = existing == state.materials.id_to_index.end();
		if (is_new && state.materials.items.length() >= MAX_MATERIALS)
		{
			// Meshes using it resolve to -1 (missing material), like a skipped image.
			printf("Exceeded MAX_MATERIALS (%i); skipping material UID %i\n", MAX_MATERIALS, in_pending.unique_id);
			return false;
		}
	
		Material material = {
			.base_color = in_pending.base_color,
			.emission_color = in_pending.emission_color,
			.metallic = in_pending.metallic,
			.roughness = in_pending.roughness,
			.emission_strength = in_pending.emission_strength,
			.base_color_image_index = -1,
			.emission_color_image_index = -1,
			.metallic_image_index = -1,
			.roughness_image_index = -1,
			.displacement_image_index = -1,
			.displacement_scale = in_pending.displacement_scale,
			.displacement_midpoint = in_pending.displacement_midpoint,
		};
	
		// Resolve image ids -> bindless indices (images in the same update were
		// registered just before materials; id 0 means no image).
		i32 missing_image_id = 0;
		auto resolve_image_index = [&](i32 in_image_id) -> i32
		{
			if (in_image_id <= 0)
			{
				return -1;
			}
			// Missing when register_image skipped it (malformed or over MAX_BINDLESS_IMAGES).
			auto found = state.images.id_to_index.find(in_image_id);
			if (found == state.images.id_to_index.end())
			{
				missing_image_id = in_image_id;
				return -1;
			}
			return found->second;
		};
		material.base_color_image_index = resolve_image_index(in_pending.base_color_image_id);
		material.emission_color_image_index = resolve_image_index(in_pending.emission_color_image_id);
		material.metallic_image_index = resolve_image_index(in_pending.metallic_image_id);
		material.roughness_image_index = resolve_image_index(in_pending.roughness_image_id);
		material.displacement_image_index = resolve_image_index(in_pending.displacement_image_id);

		// Material mirrors a std430 struct with explicit padding, so memcmp is exact.
		Material* slot = is_new ? nullptr : &state.materials.items[existing->second];
		if (slot && memcmp(slot, &material, sizeof(Material)) == 0)
		{
			return false;	// unchanged resend; any missing-image warning was already printed
		}

		if (missing_image_id != 0)
		{
			printf("\tMaterial UID %i: image UID %i not registered; using no texture\n",
				in_pending.unique_id, missing_image_id);
		}

		if (slot)
		{
			// The materials SSBO is one host-visible buffer that in-flight frames
			// still read; rewriting changed bytes under them is a data race.
			// shortcut: idles the GPU once per edited material, fine at editing rates; move materials to a ResizableGpuStreamRing if edits ever stream every frame.
			VK_CHECK(vulkan_device_wait_idle(&state.vk));
			*slot = material;
			return true;
		}

		state.materials.id_to_index[in_pending.unique_id] = (i32) state.materials.items.length();
		state.materials.items.add(material);
		return true;
	}
	
	void reset_materials()
	{
		state.materials.id_to_index.clear();
		state.materials.items.reset();
		// GPU buffer deliberately kept alive (bound every frame); stale contents
		// are unreachable once id_to_index is empty
	}
	
	// Maps a mesh's raw material IDS (from the wire) to registered indices
	void resolve_mesh_material_indices(Mesh& in_mesh)
	{
		for (u32 material_idx = 0; material_idx < in_mesh.material_indices_count; ++material_idx)
		{
			const i32 material_id = in_mesh.material_indices[material_idx];
			in_mesh.material_indices[material_idx] = -1;
	
			auto found = state.materials.id_to_index.find(material_id);
			if (found == state.materials.id_to_index.end())
			{
				printf("\tFailed to find material with id: %i\n", material_id);
				continue;
			}
			in_mesh.material_indices[material_idx] = found->second;
		}
	}
	
	// Drains SceneUpdate messages on the main thread. GPU buffer destruction for
	// replaced/deleted objects routes through the deletion queue, so this is safe
	// while frames are in flight. Per-message processing order is:
	// images -> materials -> objects -> deleted -> reset.
	void live_link_drain_channels()
	{
		while (optional<SceneUpdate> received_update = state.live_link.scene_updates.receive())
		{
			SceneUpdate& scene_update = *received_update;
			State::DataOrientedState::LiveLinkImportStats import_stats;
			static_cast<SceneUpdate::ImportStats&>(import_stats) = scene_update.stats;
			import_stats.update_index = state.data_oriented.last_import.update_index + 1;
			state.data_oriented.last_import = import_stats;
			state.data_oriented.import_history.add(import_stats);
			state.data_oriented.selected_import_history_index = (i32) state.data_oriented.import_history.length() - 1;
			if (scene_update.has_object_batch)
			{
				state.runtime.blender_data_loaded = true;
			}
	
			if (!state.debug_camera.live_link_initialization_complete)
			{
				state.debug_camera.live_link_initialization_complete = true;
				if (scene_update.editor_camera)
				{
					state.debug_camera.camera = *scene_update.editor_camera;
					state.debug_camera.initial_location = state.debug_camera.camera.location;
					const Camera& camera = state.debug_camera.camera;
					printf(
						"Debug camera initialized from Blender viewport: "
						"location=(%.3f, %.3f, %.3f) forward=(%.3f, %.3f, %.3f) up=(%.3f, %.3f, %.3f)\n",
						camera.location.X,
						camera.location.Y,
						camera.location.Z,
						camera.forward.X,
						camera.forward.Y,
						camera.forward.Z,
						camera.up.X,
						camera.up.Y,
						camera.up.Z
					);
				}
				else
				{
					printf("Debug camera using built-in fallback; first Live Link update had no valid Blender viewport camera\n");
				}
			}
	
			// Runtime clones borrow catalog allocations, so remove them before any
			// template in this complete Live Link batch can be replaced or deleted.
			mech_suspend_runtime_objects();
	
			// Images (before materials — register_material resolves image ids)
			for (PendingImage& pending_image : scene_update.images)
			{
				register_image(std::move(pending_image));
			}
	
			// Materials
			{
				bool materials_updated = false;
				for (const PendingMaterial& pending_material : scene_update.materials)
				{
					materials_updated = register_material(pending_material) || materials_updated;
				}
				if (materials_updated)
				{
					update_materials_buffer(state);
				}
			}
	
			// Transfer the decoded objects from the batch to the scene.
			DynamicArray<Object> updated_objects = std::move(scene_update.objects);
			for (Object& updated_object : updated_objects)
			{
				state.data_oriented.frame.live_link_updated_objects += 1;
				i32 updated_object_uid = updated_object.unique_id;

				if (updated_object.has_mesh)
				{
					resolve_mesh_material_indices(updated_object.mesh);
				}
	
				// Create the Jolt body on the drained copy BEFORE the map insert
				// so the JPH::Body* is copied into the map.
				if (updated_object.has_rigid_body)
				{
					object_add_jolt_body(updated_object);
				}
	
				// Finalize the character here too: parsing only fills settings,
				// because Jolt objects must be created on the main thread.
				if (updated_object.has_character)
				{
					updated_object.character = character_create(jolt_state, updated_object.character.settings);
				}
	
				const bool selects_player =
					updated_object.has_character && updated_object.character.settings.player_controlled;
				const bool selects_camera = updated_object.has_camera_control;
				scene_insert_or_replace_object(state, std::move(updated_object));
				if (selects_player)
				{
					state.scene.player_character_id = updated_object_uid;
				}
				if (selects_camera)
				{
					state.scene.camera_control_id = updated_object_uid;
				}
			}
	
			// Deleted objects
			for (i32 deleted_object_uid : scene_update.deleted_object_uids)
			{
				state.data_oriented.frame.live_link_deleted_objects += 1;
				if (scene_remove_object(state, deleted_object_uid))
				{
					printf("Removing object. UID: %i\n", deleted_object_uid);
				}
			}
	
			// Reset
			if (scene_update.reset)
			{
				state.data_oriented.frame.live_link_reset_count += 1;
				state.runtime.blender_data_loaded = false;
				mech_reset_all();
				scene_clear_objects(state);
				reset_materials();
				reset_images();
			}
	
			if (!scene_update.reset)
			{
				mech_reconcile_instances();
			}
		}
	}

	inline bool load_initial_file(State& in_state, const std::string& in_path)
	{
		(void) in_state;
		if (FILE* file = fopen(in_path.c_str(), "rb"))
		{
			fseek(file, 0, SEEK_END);
			const long file_size = ftell(file);
			rewind(file);
			assert(file_size > 0);

			DynamicArray<u8> flatbuffer_data;
			flatbuffer_data.add_uninitialized(file_size);
			const size_t bytes_read = fread(flatbuffer_data.data(), 1, file_size, file);
			fclose(file);
			assert(bytes_read == (size_t) file_size);

			parse_flatbuffer_data(flatbuffer_data);
			return true;
		}

		printf("Failed to open init file: %s\n", in_path.c_str());
		return false;
	}

	inline void start(State& in_state)
	{
		in_state.live_link.thread = std::thread(live_link_thread_function);
	}

	inline void drain(State& in_state)
	{
		(void) in_state;
		live_link_drain_channels();
	}

	inline void stop(State& in_state)
	{
		in_state.runtime.game_running = false;
		if (in_state.live_link.thread.joinable())
		{
			in_state.live_link.thread.join();
		}
	}

	inline void cleanup_imported_resources(State& in_state)
	{
		(void) in_state;
		reset_images();
	}
}
